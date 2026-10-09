/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <hip/hip_runtime_api.h>

#include "buffer.h"
#include "control.h"
#include "hip-seam.h"
#include "hipobj-parse.h"
#include "hipobj-private.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-wrapper.h"
#include "no-destructor.h"
#include "rdma-topology.h"
#include "state.h"
#include "token.h"
#include "transport.h"
#ifdef HIPOBJECT_V2_API
#include "v2-registry.h"
#include "v2-transport.h"
#include "v2-wire.h"
#endif

namespace hipObj {

/* Never destroyed: hipObjShutdown() releases their resources. Clang still
 * warns, since NoDestructor's empty destructor isn't trivial. */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF
HIPOBJ_WARN_NO_EXIT_DTOR_OFF
static NoDestructor<BufferMap>    g_bufferMap;
static NoDestructor<RcConnection> g_conn;
HIPOBJ_WARN_NO_EXIT_DTOR_ON
HIPOBJ_WARN_NO_GLOBAL_CTOR_ON

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
 * buffer registered at devPtr. buf is g_bufferMap->find(devPtr). */
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

static bool
buildRdmaToken(const BufferMap::BufEntry &buf, size_t size, off_t offset, RdmaToken &token)
{
    if (!buf.mr || !g_conn->qp) {
        return false;
    }
    if (!rangeInRegistration(offset, size, buf.size)) {
        return false;
    }
    token.transport  = TRANSPORT_RC;
    token.qpNum      = g_conn->qp->qp_num;
    token.gid        = toGid(g_conn->localGid);
    token.rkey       = buf.mr->rkey;
    token.remoteAddr = buf.remoteAddr + static_cast<uint64_t>(offset);
    token.length     = size;
    token.portNum    = g_conn->portNum;
    token.lid        = 0;
    return true;
}

static int
finishTransferAfterReply(const char *reply, size_t replyLen, bool requiresDeviceSync)
{
    RdmaToken peerToken{};
    int       httpCode = 0;
    if (parsePeerTokenFromReply(reply, replyLen, peerToken, httpCode)) {
        if (connectRcPeer(*g_conn, peerToken) != 0) {
            return -1;
        }
    }
    // A poll timeout or error completion is a failure: the transfer outcome
    // is unknown. With the current one-sided protocol the responder side
    // may not observe a completion at all (see issue #22), so this check
    // reports "no evidence of failure" rather than "transfer verified".
    if (pollCompletion(*g_conn, -1, 5000) != 0) {
        return -1;
    }
    if (!requiresDeviceSync) {
        return 0;
    }
    hipError_t err = hipObj::hipOps().hipDeviceSynchronize();
    return (err == hipSuccess) ? 0 : -1;
}

/* How long to wait for a staging copy before giving up. A 1 MiB copy is
 * single-digit milliseconds on real hardware and ~170 ms on the emulated GPU
 * the CI lanes use, so the default is several orders of magnitude of slack.
 * It exists for one reason: a DMA that never completes must not become an
 * unkillable process. ROCm waits on the completion signal with
 * BusyWaitSignal::WaitRelaxed, which spins in userspace rather than blocking,
 * so a wedged copy shows up as a busy core and no kernel log at all -- there
 * is no other layer that will ever time this out.
 *
 * HIPOBJ_STAGE_TIMEOUT_MS overrides the default with a whole number of
 * milliseconds. A value that isn't one, or is out of range, is ignored. */
static std::chrono::milliseconds
stageTimeout()
{
    static const std::chrono::milliseconds timeout = [] {
        constexpr std::chrono::milliseconds kDefault{30000};
        /* Longer than any copy could take, and short enough that adding it
         * to the current time can't overflow the clock */
        constexpr std::chrono::milliseconds kMax = std::chrono::hours(24);

        const char *env = std::getenv("HIPOBJ_STAGE_TIMEOUT_MS");
        if (!env) {
            return kDefault;
        }
        const auto ms = parseNumber<int64_t>(env);
        if (!ms || *ms <= 0 || *ms > kMax.count()) {
            return kDefault;
        }
        return std::chrono::milliseconds(*ms);
    }();
    return timeout;
}

/* hipMemcpy with a deadline. Async copy plus a recorded event polled to a
 * wall-clock bound: on timeout the event and the copy are abandoned
 * deliberately -- the copy owns the staging buffer and the stream, and there
 * is no safe way to reclaim either while the DMA may still land. The caller
 * gets an error instead of a hang, which is the whole point. */
static hipObjError_t
stageCopyWithDeadline(void *dev, void *host, size_t size, bool toDevice)
{
    HipOps             &ops  = hipOps();
    const hipMemcpyKind kind = toDevice ? hipMemcpyHostToDevice : hipMemcpyDeviceToHost;
    void               *dst  = toDevice ? dev : host;
    const void         *src  = toDevice ? host : dev;

    /* Unbounded fallback, for a table that cannot express the bounded form --
     * a test seam with only the classic entries, or a HIP runtime too old to
     * have events. */
    auto blockingCopy = [&]() -> hipObjError_t {
        if (!ops.hipMemcpy) {
            return {hipObjInternalError, 0};
        }
        hipError_t err = ops.hipMemcpy(dst, src, size, kind);
        return (err == hipSuccess) ? hipObjError_t{hipObjSuccess, 0} : hipObjError_t{hipObjInternalError, 0};
    };

    if (!ops.hipMemcpyAsync || !ops.hipEventCreate || !ops.hipEventRecord || !ops.hipEventQuery ||
        !ops.hipEventDestroy) {
        return blockingCopy();
    }

    hipEvent_t done = nullptr;
    if (ops.hipEventCreate(&done) != hipSuccess) {
        return blockingCopy();
    }

    hipError_t err = ops.hipMemcpyAsync(dst, src, size, kind, nullptr);
    if (err == hipSuccess) {
        err = ops.hipEventRecord(done, nullptr);
    }
    if (err != hipSuccess) {
        (void)ops.hipEventDestroy(done);
        return {hipObjInternalError, 0};
    }

    const std::chrono::milliseconds timeout  = stageTimeout();
    const auto                      deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        hipError_t q = ops.hipEventQuery(done);
        if (q == hipSuccess) {
            (void)ops.hipEventDestroy(done);
            return {hipObjSuccess, 0};
        }
        if (q != hipErrorNotReady) {
            (void)ops.hipEventDestroy(done);
            return {hipObjInternalError, 0};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            const int64_t timeoutMs = timeout.count();
            fprintf(stderr,
                    "hipObj: staging %s copy of %zu bytes did not complete within "
                    "%" PRId64 " ms; abandoning it. The GPU never signalled completion -- "
                    "see HIPOBJ_STAGE_TIMEOUT_MS.\n",
                    toDevice ? "host-to-device" : "device-to-host", size, timeoutMs);
            return {hipObjInternalError, 0};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static hipObjError_t
stageBuffer(void *devPtr, const BufferMap::BufEntry &buf, size_t size, off_t offset, bool toDevice)
{
    if (!buf.hostBuf) {
        /* the NIC reads and writes the caller's memory */
        return {hipObjSuccess, 0};
    }
    if (!rangeInRegistration(offset, size, buf.size)) {
        return {hipObjInvalidValue, 0};
    }
    void *host = static_cast<char *>(buf.hostBuf.get()) + offset;
    void *dev  = static_cast<char *>(devPtr) + offset;
    return stageCopyWithDeadline(dev, host, size, toDevice);
}

static hipObjError_t
runRdmaTransfer(const BufferMap::BufEntry &buf, size_t size, off_t offset, hipObjOps_t *ops, void *ctx)
{
    RdmaToken token{};
    if (!buildRdmaToken(buf, size, offset, token)) {
        return {hipObjRdmaError, 0};
    }
    const RdmaTokenHex encoded = encodeRdmaTokenHex(token);
    if (injectRdmaToken(ops, ctx, std::string_view(encoded.data(), kRdmaTokenHexLen)) != 0) {
        return {hipObjS3Error, 0};
    }
    char   replyBuf[512];
    size_t replyLen   = sizeof(replyBuf);
    int    rdmaStatus = 0;
    if (receiveRdmaReplyRaw(ops, ctx, replyBuf, &replyLen, rdmaStatus) != 0 || rdmaStatus != 0) {
        return {hipObjS3Error, 0};
    }
    if (finishTransferAfterReply(replyBuf, replyLen, buf.isDmabuf) != 0) {
        return {hipObjRdmaError, 0};
    }
    return {hipObjSuccess, 0};
}

#ifdef HIPOBJECT_V2_API
/* The most any S3 service allows: AWS once allowed 255-character bucket
 * names, and object keys are at most 1024 bytes */
constexpr size_t kMaxBucketLenV2 = 255;
constexpr size_t kMaxKeyLenV2    = 1024;

/* True for a non-empty string of at most maxLen bytes. Scans at most
 * maxLen + 1 bytes. */
static bool
isValidNameV2(const char *name, size_t maxLen)
{
    if (!name) {
        return false;
    }
    size_t len = strnlen(name, maxLen + 1);
    return len > 0 && len <= maxLen;
}

/* The characters RFC 3986 leaves unreserved, which a canonical query
 * never escapes */
static bool
isUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

/* The value of an uppercase hex digit, or -1 for anything else */
static int
upperHexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* True when every character of a query key or value is unreserved or part
 * of a %XX escape with uppercase hex digits for a byte that isn't
 * unreserved */
static bool
isEncodedQueryComponent(std::string_view component)
{
    for (size_t i = 0; i < component.size(); ++i) {
        const char c = component[i];
        if (isUnreserved(c)) {
            continue;
        }
        if (c != '%' || component.size() - i < 3) {
            return false;
        }
        const int hi = upperHexValue(component[i + 1]);
        const int lo = upperHexValue(component[i + 2]);
        if (hi < 0 || lo < 0 || isUnreserved(static_cast<char>((hi << 4) | lo))) {
            return false;
        }
        i += 2;
    }
    return true;
}

/* A canonical query string, in the SigV4 form the V2 target uses, is a
 * list of key=value parameters separated by '&', with the '=' present
 * even when the value is empty. Keys and values are percent-encoded with
 * uppercase hex digits, escaping only the characters that aren't
 * unreserved, and the parameters are sorted by key and then by value. The
 * string goes into the request as is, so anything else, such as a space
 * or a CR or LF, is rejected. NULL and "" mean no query. */
static bool
isValidCanonicalQueryV2(const char *query)
{
    if (!query || *query == '\0') {
        return true;
    }
    const std::string_view all(query);
    std::string_view       prevKey;
    std::string_view       prevValue;
    size_t                 start = 0;
    for (;;) {
        const size_t           end   = all.find('&', start);
        const std::string_view param = all.substr(start, end == std::string_view::npos ? end : end - start);
        const size_t           eq    = param.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        const std::string_view key   = param.substr(0, eq);
        const std::string_view value = param.substr(eq + 1);
        if (key.empty() || !isEncodedQueryComponent(key) || !isEncodedQueryComponent(value)) {
            return false;
        }
        if (start > 0 && (key < prevKey || (key == prevKey && value < prevValue))) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        prevKey   = key;
        prevValue = value;
        start     = end + 1;
    }
}

/* Checks the arguments to hipObjGetV2() and hipObjPutV2() */
static hipObjError_t
checkTransferArgsV2(const char *bucket, const char *key, const void *devPtr, uint64_t size, uint64_t offset,
                    const char *query, const hipObjOpsV2_t *ops)
{
    if (!isValidNameV2(bucket, kMaxBucketLenV2) || !isValidNameV2(key, kMaxKeyLenV2) || !devPtr ||
        size == 0 || !isValidCanonicalQueryV2(query)) {
        return {hipObjInvalidValue, 0};
    }
    if (!ops || !ops->sendPrepare || !ops->sendReady || !ops->sendCancel) {
        return {hipObjInvalidValue, 0};
    }
    if (size > v2::kMaxTransferSize) {
        return {hipObjSizeTooLarge, 0};
    }
    /* The end of the object range, offset + size, must not wrap */
    if (offset > std::numeric_limits<uint64_t>::max() - size) {
        return {hipObjInvalidValue, 0};
    }
    return {hipObjSuccess, 0};
}
#endif /* HIPOBJECT_V2_API */

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
#ifdef HIPOBJECT_V2_API
            case hipObjNotSupported:
                return "hipobj-rc-v2 not supported by server";
            case hipObjBusy:
                return "Server busy (backpressure)";
#endif /* HIPOBJECT_V2_API */
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
    hipObj::DriverState &state = hipObj::getState();
    if (state.initialized) {
        return {hipObjAlreadyInitialized, 0};
    }
    if (!hipObj::ibv.is_initialized) {
        return {hipObjRdmaError, 0};
    }
    const bool haveNicHint = config->nicHint && config->nicHint[0] != '\0';
    int        gpuDevice   = config->gpuDevice;
    if (gpuDevice < 0) {
        hipError_t err = hipObj::hipOps().hipGetDevice(&gpuDevice);
        if (err != hipSuccess) {
            // No GPU to infer a device from. That is only fatal when we also have
            // no NIC hint -- with a hint the topology lookup below is skipped
            // entirely, so an absent GPU is not an error.
            if (err != hipErrorNoDevice || !haveNicHint) {
                return {hipObjRdmaError, static_cast<int>(err)};
            }
            gpuDevice = -1;
        }
    }
    else {
        // An explicit device has to exist, even when a NIC hint means its
        // topology isn't needed
        int        deviceCount = 0;
        hipError_t err         = hipObj::hipOps().hipGetDeviceCount(&deviceCount);
        if (err == hipErrorNoDevice) {
            deviceCount = 0;
        }
        else if (err != hipSuccess) {
            return {hipObjRdmaError, static_cast<int>(err)};
        }
        if (gpuDevice >= deviceCount) {
            return {hipObjInvalidValue, 0};
        }
    }
    const char *devName  = nullptr;
    int         nicIndex = -1;
    if (gpuDevice >= 0) {
        nicIndex =
            hipObj::GetClosestNicToGpu(gpuDevice, config->nicHint ? config->nicHint : nullptr, &devName);
    }
    if (nicIndex < 0) {
        // GPU topology lookup failed (no GPU or no matching NIC). When a NIC name
        // hint is provided, try opening it directly without GPU topology so the
        // library works in GPU-less environments (e.g. CI with emulated RDMA).
        if (haveNicHint) {
            devName  = config->nicHint;
            nicIndex = 0;
        }
        else {
            return {hipObjNicNotFound, 0};
        }
    }
    int ret = (devName) ? hipObj::openRdmaDeviceByName(devName, *hipObj::g_conn)
                        : hipObj::openRdmaDevice(nicIndex, *hipObj::g_conn);
    if (ret != 0) {
        return {hipObjRdmaError, 0};
    }
    ret = hipObj::createRcQp(*hipObj::g_conn, 256, 128, 128);
    if (ret != 0) {
        hipObj::closeRdmaDevice(*hipObj::g_conn);
        return {hipObjRdmaError, 0};
    }
    ret = hipObj::transitionQpToInit(*hipObj::g_conn);
    if (ret != 0) {
        hipObj::closeRdmaDevice(*hipObj::g_conn);
        return {hipObjRdmaError, 0};
    }
    state.initialized = true;
    state.gpuDevice   = gpuDevice;
    state.endpoint    = config->endpoint ? config->endpoint : "";
    state.region      = config->region ? config->region : "";
    state.nicHint     = config->nicHint ? config->nicHint : "";
    state.nicIndex    = nicIndex;
    state.flags       = config->flags;
    return {hipObjSuccess, 0};
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjSuccess, 0};
    }
#ifdef HIPOBJECT_V2_API
    /* v2 first: release every connection (destroy retries included);
     * leftover poison must stop the teardown so the failure is
     * visible instead of violating the PD/context lifetime rule. */
    bool                            poisonLeft = false;
    hipObj::v2::ConnectionRegistry &reg        = hipObj::v2::registry();
    std::vector<hipObj::v2::ConnId> ids;
    reg.forEachId([&ids](hipObj::v2::ConnId id) { ids.push_back(id); });
    for (auto id : ids) {
        int rc = hipObj::v2::releaseConnection(id);
        if (rc == hipObj::v2::kReleaseLeftover) {
            poisonLeft = true;
        }
    }
    if (poisonLeft || reg.size() > 0) {
        return {hipObjRdmaError, 0};
    }
#endif /* HIPOBJECT_V2_API */
    hipObj::g_bufferMap->deregisterAll();
    hipObj::closeRdmaDevice(*hipObj::g_conn);
    state.initialized = false;
    state.gpuDevice   = 0;
    state.endpoint.clear();
    state.region.clear();
    state.nicHint.clear();
    state.nicIndex = -1;
    state.flags    = 0;
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    hipObjError_t err = hipObj::checkRegistrationArgs(devPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    if (!hipObj::isValidDeviceRange(devPtr, size)) {
        return {hipObjInvalidValue, 0};
    }
    if (hipObj::g_bufferMap->isRegistered(devPtr)) {
        return {hipObjBufAlreadyRegistered, 0};
    }
    int ret = hipObj::g_bufferMap->registerBuffer(devPtr, size, hipObj::g_conn->pd.get());
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    hipObjError_t err = hipObj::checkRegistrationArgs(hostPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    if (hipObj::g_bufferMap->isRegistered(hostPtr)) {
        return {hipObjBufAlreadyRegistered, 0};
    }
    int ret = hipObj::g_bufferMap->registerHostBuffer(hostPtr, size, hipObj::g_conn->pd.get());
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!devPtr) {
        return {hipObjInvalidValue, 0};
    }
    if (!hipObj::g_bufferMap->isRegistered(devPtr)) {
        return {hipObjBufNotRegistered, 0};
    }
    int ret = hipObj::g_bufferMap->deregisterBuffer(devPtr);
    if (ret != 0) {
        return {hipObjRdmaError, 0};
    }
    return {hipObjSuccess, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjGet(hipObjHandle_t handle, void *devPtr, size_t size, off_t offset, hipObjOps_t *ops, void *ctx)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    (void)handle;
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!ops || !ops->sendRequest || !ops->recvReply) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = hipObj::g_bufferMap->find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::runRdmaTransfer(*buf, size, offset, ops, ctx);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::stageBuffer(devPtr, *buf, size, offset, true);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjPut(hipObjHandle_t handle, const void *devPtr, size_t size, off_t offset, hipObjOps_t *ops, void *ctx)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    (void)handle;
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!ops || !ops->sendRequest || !ops->recvReply) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = hipObj::g_bufferMap->find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    err = hipObj::stageBuffer(const_cast<void *>(devPtr), *buf, size, offset, false);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::runRdmaTransfer(*buf, size, offset, ops, ctx);
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (direction != HIPOBJ_SYNC_TO_HOST && direction != HIPOBJ_SYNC_TO_DEVICE) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = hipObj::g_bufferMap->find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, offset);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::stageBuffer(devPtr, *buf, size, offset, direction == HIPOBJ_SYNC_TO_DEVICE);
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!outToken || (op != HIPOBJ_RDMA_OP_PUT && op != HIPOBJ_RDMA_OP_GET)) {
        return {hipObjInvalidValue, 0};
    }
    const auto   *buf = hipObj::g_bufferMap->find(devPtr);
    hipObjError_t err = hipObj::checkBufferArgs(devPtr, buf, size, 0);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    hipObj::RdmaToken token{};
    if (!hipObj::buildRdmaToken(*buf, size, 0, token)) {
        return {hipObjRdmaError, 0};
    }
    const hipObj::RdmaTokenHex encoded = hipObj::encodeRdmaTokenHex(token);
    char                      *copy    = static_cast<char *>(std::malloc(encoded.size()));
    if (!copy) {
        return {hipObjInternalError, 0};
    }
    std::memcpy(copy, encoded.data(), encoded.size());
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
    int code = 0;
    if (!hipObj::parseRdmaReplyHttpCode(reply, replyLen, code)) {
        return {hipObjInvalidValue, 0};
    }
    *httpCode = code;
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

#ifdef HIPOBJECT_V2_API
// Not implemented yet. The arguments are checked, then callers are told
// to fall back to the v1 RDMA path.
hipObjError_t
hipObjPutV2(const char *bucket, const char *key, const void *devPtr, uint64_t size, uint64_t offset,
            const char *query, hipObjOpsV2_t *ops, void *ctx)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    (void)ctx;
    hipObjError_t err = hipObj::checkTransferArgsV2(bucket, key, devPtr, size, offset, query, ops);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return {hipObjNotSupported, 0};
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjGetV2(const char *bucket, const char *key, void *devPtr, uint64_t size, uint64_t offset,
            const char *query, hipObjOpsV2_t *ops, void *ctx)
try {
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
    (void)ctx;
    hipObjError_t err = hipObj::checkTransferArgsV2(bucket, key, devPtr, size, offset, query, ops);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return {hipObjNotSupported, 0};
}
catch (...) {
    return hipObj::handleException();
}
#endif /* HIPOBJECT_V2_API */

} // extern "C"
