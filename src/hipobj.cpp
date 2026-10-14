/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <hip/hip_runtime_api.h>

#include "buffer.h"
#include "control.h"
#include "hip-seam.h"
#include "hipobj-private.h"
#include "hipobj-span.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "state.h"
#include "token.h"
#include "transport.h"

namespace hipObj {

static hipObjError_t
handleException()
{
    try {
        throw;
    }
    catch (const std::exception &) {
        return {hipObjInternalError, 0};
    }
    catch (...) {
        return {hipObjInternalError, 0};
    }
}

/* Returned by a locked entry point when a callback calls it, where taking
 * the API lock again would deadlock (see ApiGuard) */
constexpr hipObjError_t kReentryError = {hipObjInvalidValue, 0};

/* True when [offset, offset + size) lies inside a registration of regSize
 * bytes. Compares without computing offset + size, which could wrap. */
static bool
rangeInRegistration(off_t offset, size_t size, size_t regSize)
{
    if (offset < 0 || std::cmp_greater(offset, regSize)) {
        return false;
    }
    return size <= regSize - static_cast<size_t>(offset);
}

/* Checks the arguments to a call that registers [ptr, ptr + size) */
static hipObjError_t
checkRegistrationArgs(const void *ptr, size_t size)
{
    if (!ptr || size == 0) {
        return {hipObjInvalidValue, 0};
    }
    if (size > MAX_MR_SIZE) {
        return {hipObjSizeTooLarge, 0};
    }
    /* The end of the buffer, ptr + size, must not wrap */
    if (size > std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(ptr)) {
        return {hipObjInvalidValue, 0};
    }
    return {hipObjSuccess, 0};
}

/* Checks the arguments to a call that uses [offset, offset + size) of the
 * buffer registered at devPtr. buf is the instance's buffers.find(devPtr). */
static hipObjError_t
checkBufferArgs(const void *devPtr, const BufferMap::BufEntry *buf, size_t size, off_t offset)
{
    if (!devPtr || size == 0 || offset < 0) {
        return {hipObjInvalidValue, 0};
    }
    if (!buf) {
        return {hipObjBufNotRegistered, 0};
    }
    if (!rangeInRegistration(offset, size, buf->size)) {
        return {hipObjInvalidValue, 0};
    }
    return {hipObjSuccess, 0};
}

static std::optional<RdmaToken>
buildRdmaToken(const RcConnection &conn, const BufferMap::BufEntry &buf, size_t size, off_t offset)
{
    if (!buf.mr || !conn.qp) {
        return std::nullopt;
    }
    if (!rangeInRegistration(offset, size, buf.size)) {
        return std::nullopt;
    }
    RdmaToken token;
    token.transport  = TransportType::RC;
    token.qpNum      = conn.qp->qp_num;
    token.gid        = toGid(conn.localGid);
    token.rkey       = buf.mr->rkey;
    token.remoteAddr = buf.remoteAddr + static_cast<uint64_t>(offset);
    token.length     = size;
    token.portNum    = conn.portNum;
    token.lid        = 0;
    return token;
}

static int
finishTransferAfterReply(RcConnection &conn, std::string_view reply, bool requiresDeviceSync,
                         std::chrono::milliseconds pollTimeout)
{
    if (const auto peerToken = parsePeerTokenFromReply(reply)) {
        if (connectRcPeer(conn, *peerToken) != 0) {
            return -1;
        }
    }
    // A poll timeout or error completion is a failure: the transfer outcome
    // is unknown. With the current one-sided protocol the responder side
    // may not observe a completion at all (see issue #22), so this check
    // reports "no evidence of failure" rather than "transfer verified".
    if (pollCompletion(conn, -1, pollTimeout) != 0) {
        return -1;
    }
    if (!requiresDeviceSync) {
        return 0;
    }
    hipError_t err = hip().deviceSynchronize();
    return (err == hipSuccess) ? 0 : -1;
}

/* hipMemcpy with a deadline (see Options::stageTimeout). Async copy plus a
 * recorded event polled to a wall-clock bound: on timeout the event and the
 * copy are abandoned deliberately -- the copy owns the staging buffer and
 * the stream, and there is no safe way to reclaim either while the DMA may
 * still land. The caller gets an error instead of a hang, which is the
 * whole point. */
static hipObjError_t
stageCopyWithDeadline(void *dev, void *host, size_t size, bool toDevice, std::chrono::milliseconds timeout)
{
    const HipRuntime   &runtime = hip();
    const hipMemcpyKind kind    = toDevice ? hipMemcpyHostToDevice : hipMemcpyDeviceToHost;
    void               *dst     = toDevice ? dev : host;
    const void         *src     = toDevice ? host : dev;

    hipEvent_t rawDone = nullptr;
    if (runtime.eventCreate(&rawDone) != hipSuccess) {
        return {hipObjInternalError, 0};
    }
    HipEventPtr done(rawDone);

    hipError_t err = runtime.memcpyAsync(dst, src, size, kind, nullptr);
    if (err == hipSuccess) {
        err = runtime.eventRecord(done.get(), nullptr);
    }
    if (err != hipSuccess) {
        return {hipObjInternalError, 0};
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        hipError_t q = runtime.eventQuery(done.get());
        if (q == hipSuccess) {
            return {hipObjSuccess, 0};
        }
        if (q != hipErrorNotReady) {
            return {hipObjInternalError, 0};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            /* The copy may still signal the event, so it's leaked rather
             * than destroyed */
            dropOwnership(done);
            const int64_t     timeoutMs = timeout.count();
            const std::string direction = toDevice ? "host-to-device" : "device-to-host";
            fprintf(stderr,
                    "hipObj: staging %s copy of %zu bytes did not complete within "
                    "%" PRId64 " ms; abandoning it. The GPU never signalled completion -- "
                    "see HIPOBJ_STAGE_TIMEOUT_MS.\n",
                    direction.c_str(), size, timeoutMs);
            return {hipObjInternalError, 0};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static hipObjError_t
stageBuffer(void *devPtr, const BufferMap::BufEntry &buf, size_t size, off_t offset, bool toDevice,
            std::chrono::milliseconds timeout)
{
    if (!buf.hostBuf) {
        /* the NIC reads and writes the caller's memory */
        return {hipObjSuccess, 0};
    }
    if (!rangeInRegistration(offset, size, buf.size)) {
        return {hipObjInvalidValue, 0};
    }
    /* The device memory is only viewed for its bounds, never read here */
    const std::span<std::byte> host  = spanFromPointer(static_cast<std::byte *>(buf.hostBuf.get()), buf.size);
    const std::span<std::byte> dev   = spanFromPointer(static_cast<std::byte *>(devPtr), buf.size);
    const auto                 start = static_cast<size_t>(offset);
    return stageCopyWithDeadline(dev.subspan(start, size).data(), host.subspan(start, size).data(), size,
                                 toDevice, timeout);
}

static hipObjError_t
runRdmaTransfer(Instance &instance, const BufferMap::BufEntry &buf, size_t size, off_t offset,
                hipObjOps_t *ops, void *ctx)
{
    const auto token = buildRdmaToken(instance.conn, buf, size, offset);
    if (!token) {
        return {hipObjRdmaError, 0};
    }
    const RdmaTokenHex encoded = encodeRdmaTokenHex(*token);
    if (injectRdmaToken(ops, ctx, asStringView(std::span(encoded).first<kRdmaTokenHexLen>())) != 0) {
        return {hipObjS3Error, 0};
    }
    std::array<char, 512> replyBuf;
    const auto            reply = receiveRdmaReplyRaw(ops, ctx, replyBuf);
    if (!reply || reply->status != RdmaReplyStatus::Success) {
        return {hipObjS3Error, 0};
    }
    if (finishTransferAfterReply(instance.conn, reply->text, buf.isDmabuf, instance.options.pollTimeout) !=
        0) {
        return {hipObjRdmaError, 0};
    }
    return {hipObjSuccess, 0};
}

} // namespace hipObj

extern "C" {

const char *
hipObjGetErrorString(hipObjOpError_t err)
{
    try {
        switch (err) {
            case hipObjSuccess:
                return "Success";
            case hipObjInvalidValue:
                return "Invalid value";
            case hipObjNotInitialized:
                return "Not initialized";
            case hipObjAlreadyInitialized:
                return "Already initialized";
            case hipObjRdmaError:
                return "RDMA error";
            case hipObjS3Error:
                return "S3 error";
            case hipObjBufNotRegistered:
                return "Buffer not registered";
            case hipObjBufAlreadyRegistered:
                return "Buffer already registered";
            case hipObjNicNotFound:
                return "NIC not found";
            case hipObjDmabufNotSupported:
                return "dmabuf not supported";
            case hipObjSizeTooLarge:
                return "Size too large";
            case hipObjInternalError:
                return "Internal error";
            default:
                return "Unknown error";
        }
    }
    catch (...) {
        return "Unknown error";
    }
}

hipObjError_t
hipObjInit(hipObjConfig_t *config)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    // flags is reserved, and -1 is the only negative gpuDevice
    if (!config || config->flags != 0 || config->gpuDevice < -1) {
        return {hipObjInvalidValue, 0};
    }
    if (hipObj::currentInstance()) {
        return {hipObjAlreadyInitialized, 0};
    }
    if (!hipObj::ibvAvailable()) {
        return {hipObjRdmaError, 0};
    }
    /* std::getenv is called through a lambda, since C++ doesn't allow
     * taking the address of most standard library functions */
    return hipObj::initWith(
        *config, hipObj::systemPlatform(),
        hipObj::parseOptions([](const char *name) noexcept { return std::getenv(name); }));
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjShutdown(void)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    hipObj::shutdownInstance();
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjBufRegister(void *devPtr, size_t size)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    hipObjError_t err = hipObj::checkRegistrationArgs(devPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    if (!hipObj::isValidDeviceRange(devPtr, size)) {
        return {hipObjInvalidValue, 0};
    }
    if (instance->buffers.isRegistered(devPtr)) {
        return {hipObjBufAlreadyRegistered, 0};
    }
    int ret = instance->buffers.registerBuffer(devPtr, size, instance->conn.pd.get());
    if (ret != 0) {
        return {hipObjRdmaError, 0};
    }
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjBufRegisterHost(void *hostPtr, size_t size)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    hipObjError_t err = hipObj::checkRegistrationArgs(hostPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    if (instance->buffers.isRegistered(hostPtr)) {
        return {hipObjBufAlreadyRegistered, 0};
    }
    int ret = instance->buffers.registerHostBuffer(hostPtr, size, instance->conn.pd.get());
    if (ret != 0) {
        return {hipObjRdmaError, 0};
    }
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjBufDeregister(void *devPtr)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    if (!devPtr) {
        return {hipObjInvalidValue, 0};
    }
    if (!instance->buffers.isRegistered(devPtr)) {
        return {hipObjBufNotRegistered, 0};
    }
    int ret = instance->buffers.deregisterBuffer(devPtr);
    if (ret != 0) {
        return {hipObjRdmaError, 0};
    }
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjGet([[maybe_unused]] hipObjHandle_t handle, void *devPtr, size_t size, off_t offset, hipObjOps_t *ops,
          void *ctx)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    if (!ops || !ops->sendRequest || !ops->recvReply) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::runRdmaTransfer(*instance, *buf, size, offset, ops, ctx);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::stageBuffer(devPtr, *buf, size, offset, true, instance->options.stageTimeout);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjPut([[maybe_unused]] hipObjHandle_t handle, const void *devPtr, size_t size, off_t offset,
          hipObjOps_t *ops, void *ctx)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    if (!ops || !ops->sendRequest || !ops->recvReply) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::stageBuffer(const_cast<void *>(devPtr), *buf, size, offset, false,
                              instance->options.stageTimeout);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::runRdmaTransfer(*instance, *buf, size, offset, ops, ctx);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjBufSync(void *devPtr, size_t size, off_t offset, int direction)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    if (direction != HIPOBJ_SYNC_TO_HOST && direction != HIPOBJ_SYNC_TO_DEVICE) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::stageBuffer(devPtr, *buf, size, offset, direction == HIPOBJ_SYNC_TO_DEVICE,
                               instance->options.stageTimeout);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjGetRdmaToken(const void *devPtr, size_t size, int op, char **outToken)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return {hipObjNotInitialized, 0};
    }
    if (!outToken || (op != HIPOBJ_RDMA_OP_PUT && op != HIPOBJ_RDMA_OP_GET)) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, 0);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    const auto token = hipObj::buildRdmaToken(instance->conn, *buf, size, 0);
    if (!token) {
        return {hipObjRdmaError, 0};
    }
    const hipObj::RdmaTokenHex encoded = hipObj::encodeRdmaTokenHex(*token);
    char                      *copy    = static_cast<char *>(std::malloc(encoded.size()));
    if (!copy) {
        return {hipObjInternalError, 0};
    }
    std::ranges::copy(encoded, hipObj::spanFromPointer(copy, encoded.size()).begin());
    *outToken = copy;
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjPutRdmaToken(char *token)
try {
    if (!token) {
        return {hipObjInvalidValue, 0};
    }
    std::free(token);
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjParseRdmaReply(const char *reply, size_t replyLen, int *httpCode)
try {
    if (!reply || !httpCode) {
        return {hipObjInvalidValue, 0};
    }
    const auto code =
        hipObj::parseRdmaReplyHttpCode(hipObj::asStringView(hipObj::spanFromPointer(reply, replyLen)));
    if (!code) {
        return {hipObjInvalidValue, 0};
    }
    *httpCode = *code;
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjTokenClientNic(const char *token, char *nicIp, size_t nicIpLen)
try {
    if (!token || !nicIp || nicIpLen == 0) {
        return {hipObjInvalidValue, 0};
    }
    if (!hipObj::parseClientNicFromTokenHex(token, nicIp, nicIpLen)) {
        return {hipObjInvalidValue, 0};
    }
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

const char *
hipObjGetVersionString(void)
try {
    /* Formatted once: C++ runs a function-local static's initializer
     * exactly once, even when several threads call this at the same time */
    static const std::array<char, 32> version = [] {
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), "%d.%d.%d", HIPOBJ_VERSION_MAJOR, HIPOBJ_VERSION_MINOR,
                      HIPOBJ_VERSION_PATCH);
        return buf;
    }();
    return version.data();
}
catch (...) {
    return "0.0.0";
}

} // extern "C"
