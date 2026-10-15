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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <hip/hip_runtime_api.h>

#include "abandoned-copies.h"
#include "buffer.h"
#include "control.h"
#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj-private.h"
#include "hipobj-span.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "state.h"
#include "token.h"
#include "transport.h"

namespace hipObj {

namespace {

    /* Returned by a public function when it catches an exception, whatever
     * its type */
    constexpr hipObjError_t kExceptionError = makeError(hipObjInternalError);

    /* Returned by a locked entry point when a callback calls it, where taking
     * the API lock again would deadlock (see ApiGuard) */
    constexpr hipObjError_t kReentryError = makeError(hipObjInvalidValue);

    /* True when [offset, offset + size) lies inside a registration of regSize
     * bytes. Compares without computing offset + size, which could wrap. */
    bool rangeInRegistration(off_t offset, size_t size, size_t regSize)
    {
        if (offset < 0 || std::cmp_greater(offset, regSize)) {
            return false;
        }
        return size <= regSize - static_cast<size_t>(offset);
    }

    /* Checks the arguments to a call that registers [ptr, ptr + size) */
    hipObjError_t checkRegistrationArgs(const void *ptr, size_t size)
    {
        if (ptr == nullptr || size == 0) {
            return makeError(hipObjInvalidValue);
        }
        if (size > MAX_MR_SIZE) {
            return makeError(hipObjSizeTooLarge);
        }
        /* The end of the buffer, ptr + size, must not wrap */
        if (size > std::numeric_limits<uintptr_t>::max() - pointerToAddress(ptr)) {
            return makeError(hipObjInvalidValue);
        }
        return makeError(hipObjSuccess);
    }

    /* What a registration call returns for what the buffer map did */
    hipObjError_t registrationError(BufferMap::RegisterResult result)
    {
        if (result == BufferMap::RegisterResult::Registered) {
            return makeError(hipObjSuccess);
        }
        if (result == BufferMap::RegisterResult::AlreadyRegistered) {
            return makeError(hipObjBufAlreadyRegistered);
        }
        return makeError(hipObjRdmaError);
    }

    /* Checks the arguments to a call that uses [offset, offset + size) of the
     * buffer registered at devPtr. buf is the instance's buffers.find(devPtr). */
    hipObjError_t checkBufferArgs(const void *devPtr, const BufferMap::BufEntry *buf, size_t size,
                                  off_t offset)
    {
        if (devPtr == nullptr || size == 0 || offset < 0) {
            return makeError(hipObjInvalidValue);
        }
        if (buf == nullptr) {
            return makeError(hipObjBufNotRegistered);
        }
        if (!rangeInRegistration(offset, size, buf->size)) {
            return makeError(hipObjInvalidValue);
        }
        return makeError(hipObjSuccess);
    }

    std::optional<RdmaToken> buildRdmaToken(const RcConnection &conn, const BufferMap::BufEntry &buf,
                                            size_t size, off_t offset)
    {
        if (!buf.mr) {
            return std::nullopt;
        }
        if (!rangeInRegistration(offset, size, buf.size)) {
            return std::nullopt;
        }
        return RdmaToken{
            .qpNum      = conn.qp()->qp_num,
            .gid        = toGid(conn.device().localGid()),
            .rkey       = buf.mr->rkey,
            .remoteAddr = buf.remoteAddr + static_cast<uint64_t>(offset),
            .length     = size,
            .transport  = TransportType::RC,
            .portNum    = conn.device().portNum(),
            .lid        = 0,
        };
    }

    [[nodiscard]] bool finishTransferAfterReply(const HipRuntime &runtime, RcConnection &conn,
                                                std::string_view reply, bool requiresDeviceSync,
                                                std::chrono::milliseconds pollTimeout)
    {
        if (const auto peerToken = parsePeerTokenFromReply(reply)) {
            if (!connectRcPeer(conn, *peerToken)) {
                return false;
            }
        }
        // A poll timeout or error completion is a failure: the transfer outcome
        // is unknown. With the current one-sided protocol the responder side
        // may not observe a completion at all (see issue #22), so this check
        // reports "no evidence of failure" rather than "transfer verified".
        if (!pollCompletion(conn, std::nullopt, pollTimeout)) {
            return false;
        }
        if (!requiresDeviceSync) {
            return true;
        }
        return runtime.deviceSynchronize() == hipSuccess;
    }

    /* hipMemcpy with a deadline (see Options::stageTimeout), between the
     * caller's memory and the staging buffer of buf. Async copy plus a
     * recorded event polled to a wall-clock bound. On timeout, the caller
     * gets an error instead of a hang, which is the whole point. The copy
     * may still land, and there's no safe way to stop it, so the event and
     * the registration are handed to the instance's abandoned copies,
     * which keep the staging buffer until the copy is over. */
    hipObjError_t stageCopyWithDeadline(Instance                                         &instance,
                                        const std::shared_ptr<const BufferMap::BufEntry> &buf, void *dst,
                                        const void *src, size_t size, hipMemcpyKind kind)
    {
        const HipRuntime &runtime = instance.platform->hip();
        hipEvent_t        rawDone = nullptr;
        if (runtime.eventCreate(&rawDone) != hipSuccess) {
            return makeError(hipObjInternalError);
        }
        auto done = own<HipEventPtr>(runtime, rawDone);

        /* The calling thread's default stream, not the null stream, which
         * waits for every other blocking stream on the device, so copies from
         * different threads would wait for each other */
        hipError_t err = runtime.memcpyAsync(dst, src, size, kind, hipStreamPerThread);
        if (err == hipSuccess) {
            err = runtime.eventRecord(done.get(), hipStreamPerThread);
        }
        if (err != hipSuccess) {
            return makeError(hipObjInternalError);
        }

        const std::chrono::milliseconds timeout  = instance.options.stageTimeout;
        const auto                      deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const hipError_t q = runtime.eventQuery(done.get());
            if (q == hipSuccess) {
                return makeError(hipObjSuccess);
            }
            if (q != hipErrorNotReady) {
                return makeError(hipObjInternalError);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                instance.abandoned.add(std::move(done), buf);
                const int64_t     timeoutMs = timeout.count();
                const std::string direction =
                    kind == hipMemcpyHostToDevice ? "host-to-device" : "device-to-host";
                fprintf(stderr,
                        "hipObj: staging %s copy of %zu bytes did not complete within "
                        "%" PRId64 " ms; abandoning it. The GPU never signalled completion -- "
                        "see HIPOBJ_STAGE_TIMEOUT_MS.\n",
                        direction.c_str(), size, timeoutMs);
                return makeError(hipObjInternalError);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /* hipObjBusy when a staging copy of buf that timed out is still
     * running, since a copy or transfer that used the staging buffer could
     * mix its data with the old copy's */
    hipObjError_t checkNoAbandonedCopy(Instance &instance, const BufferMap::BufEntry &buf)
    {
        if (buf.hostBuf && instance.abandoned.running(buf)) {
            return makeError(hipObjBusy);
        }
        return makeError(hipObjSuccess);
    }

    /* Copies [offset, offset + size) of a host-staged registration from its
     * staging buffer to the device memory at devPtr */
    hipObjError_t stageToDevice(Instance &instance, void *devPtr,
                                const std::shared_ptr<const BufferMap::BufEntry> &buf, size_t size,
                                off_t offset)
    {
        if (!buf->hostBuf) {
            /* the NIC writes the caller's memory */
            return makeError(hipObjSuccess);
        }
        if (!rangeInRegistration(offset, size, buf->size)) {
            return makeError(hipObjInvalidValue);
        }
        /* The device memory is only viewed for its bounds, never accessed here */
        const auto host  = spanFromPointer(static_cast<const std::byte *>(buf->hostBuf.get()), buf->size);
        const auto dev   = spanFromPointer(static_cast<std::byte *>(devPtr), buf->size);
        const auto start = static_cast<size_t>(offset);
        return stageCopyWithDeadline(instance, buf, dev.subspan(start, size).data(),
                                     host.subspan(start, size).data(), size, hipMemcpyHostToDevice);
    }

    /* Copies [offset, offset + size) of a host-staged registration from the
     * device memory at devPtr to its staging buffer */
    hipObjError_t stageToHost(Instance &instance, const void *devPtr,
                              const std::shared_ptr<const BufferMap::BufEntry> &buf, size_t size,
                              off_t offset)
    {
        if (!buf->hostBuf) {
            /* the NIC reads the caller's memory */
            return makeError(hipObjSuccess);
        }
        if (!rangeInRegistration(offset, size, buf->size)) {
            return makeError(hipObjInvalidValue);
        }
        /* The device memory is only viewed for its bounds, never accessed here */
        const auto host  = spanFromPointer(static_cast<std::byte *>(buf->hostBuf.get()), buf->size);
        const auto dev   = spanFromPointer(static_cast<const std::byte *>(devPtr), buf->size);
        const auto start = static_cast<size_t>(offset);
        return stageCopyWithDeadline(instance, buf, host.subspan(start, size).data(),
                                     dev.subspan(start, size).data(), size, hipMemcpyDeviceToHost);
    }

    hipObjError_t runRdmaTransfer(Instance &instance, const BufferMap::BufEntry &buf, size_t size,
                                  off_t offset, const hipObjOps_t &ops, void *ctx)
    {
        const auto token = buildRdmaToken(instance.conn, buf, size, offset);
        if (!token) {
            return makeError(hipObjRdmaError);
        }
        const RdmaTokenHex encoded = encodeRdmaTokenHex(*token);
        if (!injectRdmaToken(ops, ctx, asStringView(std::span(encoded).first<kRdmaTokenHexLen>()))) {
            return makeError(hipObjS3Error);
        }
        std::array<char, 512> replyBuf{};
        const auto            reply = receiveRdmaReplyRaw(ops, ctx, replyBuf);
        if (!reply || reply->status != RdmaReplyStatus::Success) {
            return makeError(hipObjS3Error);
        }
        if (!finishTransferAfterReply(instance.platform->hip(), instance.conn, reply->text, buf.isDmabuf,
                                      instance.options.pollTimeout)) {
            return makeError(hipObjRdmaError);
        }
        return makeError(hipObjSuccess);
    }

} // namespace

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
            case hipObjBusy:
                return "Busy";
            default:
                return "Unknown error";
        }
    }
    catch (...) {
        return "Unknown error";
    }
}

/* config is only read, but it's a pointer to non-const in the public
 * header, which matches cuObject's API */
// NOLINTBEGIN(misc-const-correctness)
hipObjError_t
hipObjInit(hipObjConfig_t *config)
// NOLINTEND(misc-const-correctness)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    // flags is reserved, and -1 is the only negative gpuDevice
    if (config == nullptr || config->flags != 0 || config->gpuDevice < -1) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    if (hipObj::currentInstance() != nullptr) {
        return hipObj::makeError(hipObjAlreadyInitialized);
    }
    std::shared_ptr<const hipObj::Platform> platform = hipObj::systemPlatform();
    if (!platform) {
        return hipObj::makeError(hipObjRdmaError);
    }
    /* std::getenv is called through a lambda, since C++ doesn't allow
     * taking the address of most standard library functions */
    return hipObj::initWith(*config, std::move(platform), hipObj::parseOptions([](const char *name) noexcept {
        return std::getenv(name);
    }));
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjShutdown(void)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    hipObj::shutdownInstance();
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
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
        return hipObj::makeError(hipObjNotInitialized);
    }
    const hipObjError_t err = hipObj::checkRegistrationArgs(devPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    if (!hipObj::isValidDeviceRange(instance->platform->hip(), devPtr, size)) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    return hipObj::registrationError(instance->buffers.registerBuffer(devPtr, size, instance->device->pd()));
}
catch (...) {
    return hipObj::kExceptionError;
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
        return hipObj::makeError(hipObjNotInitialized);
    }
    const hipObjError_t err = hipObj::checkRegistrationArgs(hostPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::registrationError(
        instance->buffers.registerHostBuffer(hostPtr, size, instance->device->pd()));
}
catch (...) {
    return hipObj::kExceptionError;
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
        return hipObj::makeError(hipObjNotInitialized);
    }
    if (devPtr == nullptr) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    /* Releases the staging copies that timed out and have finished since,
     * so the buffers they used can be deregistered */
    instance->abandoned.reclaim();
    const auto result = instance->buffers.deregisterBuffer(devPtr);
    if (result == hipObj::BufferMap::DeregisterResult::Deregistered) {
        return hipObj::makeError(hipObjSuccess);
    }
    if (result == hipObj::BufferMap::DeregisterResult::NotRegistered) {
        return hipObj::makeError(hipObjBufNotRegistered);
    }
    /* Busy: a staging copy that timed out is still using the buffer. No
     * other call can be, since this one holds the API lock. */
    return hipObj::makeError(hipObjBusy);
}
catch (...) {
    return hipObj::kExceptionError;
}

/* ops is only read, but it's a pointer to non-const in the public header,
 * which matches cuObject's API */
// NOLINTBEGIN(misc-const-correctness)
hipObjError_t
hipObjGet([[maybe_unused]] hipObjHandle_t handle, void *devPtr, size_t size, off_t offset, hipObjOps_t *ops,
          void *ctx)
// NOLINTEND(misc-const-correctness)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return hipObj::makeError(hipObjNotInitialized);
    }
    if (ops == nullptr || ops->sendRequest == nullptr || ops->recvReply == nullptr) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    const auto    buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf.get(), size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::checkNoAbandonedCopy(*instance, *buf);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::runRdmaTransfer(*instance, *buf, size, offset, *ops, ctx);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::stageToDevice(*instance, devPtr, buf, size, offset);
}
catch (...) {
    return hipObj::kExceptionError;
}

/* ops is only read, but it's a pointer to non-const in the public header,
 * which matches cuObject's API */
// NOLINTBEGIN(misc-const-correctness)
hipObjError_t
hipObjPut([[maybe_unused]] hipObjHandle_t handle, const void *devPtr, size_t size, off_t offset,
          hipObjOps_t *ops, void *ctx)
// NOLINTEND(misc-const-correctness)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return hipObj::makeError(hipObjNotInitialized);
    }
    if (ops == nullptr || ops->sendRequest == nullptr || ops->recvReply == nullptr) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    const auto    buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf.get(), size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::checkNoAbandonedCopy(*instance, *buf);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::stageToHost(*instance, devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::runRdmaTransfer(*instance, *buf, size, offset, *ops, ctx);
}
catch (...) {
    return hipObj::kExceptionError;
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
        return hipObj::makeError(hipObjNotInitialized);
    }
    if (direction != HIPOBJ_SYNC_TO_HOST && direction != HIPOBJ_SYNC_TO_DEVICE) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    const auto    buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf.get(), size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::checkNoAbandonedCopy(*instance, *buf);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    if (direction == HIPOBJ_SYNC_TO_DEVICE) {
        return hipObj::stageToDevice(*instance, devPtr, buf, size, offset);
    }
    return hipObj::stageToHost(*instance, devPtr, buf, size, offset);
}
catch (...) {
    return hipObj::kExceptionError;
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
        return hipObj::makeError(hipObjNotInitialized);
    }
    if (outToken == nullptr || (op != HIPOBJ_RDMA_OP_PUT && op != HIPOBJ_RDMA_OP_GET)) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    const auto    buf = instance->buffers.find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf.get(), size, 0);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::checkNoAbandonedCopy(*instance, *buf);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    const auto token = hipObj::buildRdmaToken(instance->conn, *buf, size, 0);
    if (!token) {
        return hipObj::makeError(hipObjRdmaError);
    }
    const hipObj::RdmaTokenHex encoded = hipObj::encodeRdmaTokenHex(*token);
    char                      *copy    = static_cast<char *>(std::malloc(encoded.size()));
    if (copy == nullptr) {
        return hipObj::makeError(hipObjInternalError);
    }
    std::ranges::copy(encoded, hipObj::spanFromPointer(copy, encoded.size()).begin());
    *outToken = copy;
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjPutRdmaToken(char *token)
try {
    if (token == nullptr) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    std::free(token);
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjParseRdmaReply(const char *reply, size_t replyLen, int *httpCode)
try {
    if (reply == nullptr || httpCode == nullptr) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    const auto code =
        hipObj::parseRdmaReplyHttpCode(hipObj::asStringView(hipObj::spanFromPointer(reply, replyLen)));
    if (!code) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    *httpCode = *code;
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjTokenClientNic(const char *token, char *nicIp, size_t nicIpLen)
try {
    if (token == nullptr || nicIp == nullptr || nicIpLen == 0) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    /* Bound the scan: anything longer than a token is rejected anyway */
    if (!hipObj::parseClientNicFromTokenHex(hipObj::cStringView(token, hipObj::kRdmaTokenHexLen + 1),
                                            hipObj::spanFromPointer(nicIp, nicIpLen))) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
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
