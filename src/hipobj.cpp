/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <hip/hip_runtime.h>

#include "buffer.h"
#include "control.h"
#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-wrapper.h"
#include "rdma-topology.h"
#include "state.h"
#include "token.h"
#include "transport.h"
#ifdef HIPOBJECT_V2_API
#include "v2-registry.h"
#include "v2-transport.h"
#endif

namespace hipObj {

static BufferMap    g_bufferMap;
static RcConnection g_conn;

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

static bool
buildRdmaToken(const void *devPtr, size_t size, off_t offset, RdmaToken &token)
{
    struct ibv_mr *mr = g_bufferMap.lookupMr(const_cast<void *>(devPtr));
    if (!mr || !g_conn.qp) {
        return false;
    }
    size_t regSize = g_bufferMap.lookupSize(const_cast<void *>(devPtr));
    if (offset < 0 || static_cast<size_t>(offset) + size > regSize) {
        return false;
    }
    token.transport = TRANSPORT_RC;
    token.qpNum     = g_conn.qp->qp_num;
    std::memcpy(token.gid, g_conn.localGid.raw, 16);
    token.rkey = mr->rkey;
    token.remoteAddr =
        g_bufferMap.lookupRemoteAddr(const_cast<void *>(devPtr)) + static_cast<uint64_t>(offset);
    token.length  = size;
    token.portNum = g_conn.portNum;
    token.lid     = 0;
    return true;
}

static int
finishTransferAfterReply(const char *reply, size_t replyLen, bool requiresDeviceSync)
{
    RdmaToken peerToken{};
    int       httpCode = 0;
    if (parsePeerTokenFromReply(reply, replyLen, peerToken, httpCode)) {
        if (connectRcPeer(g_conn, peerToken) != 0) {
            return -1;
        }
    }
    // A poll timeout or error completion is a failure: the transfer outcome
    // is unknown. With the current one-sided protocol the responder side
    // may not observe a completion at all (see issue #22), so this check
    // reports "no evidence of failure" rather than "transfer verified".
    if (pollCompletion(g_conn, -1, 5000) != 0) {
        return -1;
    }
    if (!requiresDeviceSync) {
        return 0;
    }
    hipError_t err = hipObj::hipOps().hipDeviceSynchronize();
    return (err == hipSuccess) ? 0 : -1;
}

/* Milliseconds to wait for a staging copy before giving up. A 1 MiB copy is
 * single-digit milliseconds on real hardware and ~170 ms on the emulated GPU
 * the CI lanes use, so the default is several orders of magnitude of slack.
 * It exists for one reason: a DMA that never completes must not become an
 * unkillable process. ROCm waits on the completion signal with
 * BusyWaitSignal::WaitRelaxed, which spins in userspace rather than blocking,
 * so a wedged copy shows up as a busy core and no kernel log at all -- there
 * is no other layer that will ever time this out. */
static long
stageTimeoutMs()
{
    static long ms = [] {
        const char *env = getenv("HIPOBJ_STAGE_TIMEOUT_MS");
        if (!env || !*env) {
            return 30000L;
        }
        char *end = nullptr;
        long  v   = strtol(env, &end, 10);
        return (end && *end == '\0' && v > 0) ? v : 30000L;
    }();
    return ms;
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

    const long timeoutMs = stageTimeoutMs();
    const auto deadline  = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
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
            fprintf(stderr,
                    "hipObj: staging %s copy of %zu bytes did not complete within "
                    "%ld ms; abandoning it. The GPU never signalled completion -- "
                    "see HIPOBJ_STAGE_TIMEOUT_MS.\n",
                    toDevice ? "host-to-device" : "device-to-host", size, timeoutMs);
            return {hipObjInternalError, 0};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static hipObjError_t
stageBuffer(void *devPtr, size_t size, off_t offset, bool toDevice)
{
    void *hostBuf = g_bufferMap.lookupHostBuf(devPtr);
    if (!hostBuf) {
        /* the NIC reads and writes the caller's memory */
        return {hipObjSuccess, 0};
    }
    size_t regSize = g_bufferMap.lookupSize(devPtr);
    if (offset < 0 || static_cast<size_t>(offset) + size > regSize) {
        return {hipObjInvalidValue, 0};
    }
    void *host = static_cast<char *>(hostBuf) + offset;
    void *dev  = static_cast<char *>(devPtr) + offset;
    return stageCopyWithDeadline(dev, host, size, toDevice);
}

static hipObjError_t
runRdmaTransfer(const void *devPtr, size_t size, off_t offset, hipObjOps_t *ops, void *ctx)
{
    bool      requiresDeviceSync = g_bufferMap.requiresDeviceSync(const_cast<void *>(devPtr));
    RdmaToken token{};
    if (!buildRdmaToken(devPtr, size, offset, token)) {
        return {hipObjRdmaError, 0};
    }
    std::string encoded = encodeRdmaToken(token);
    if (injectRdmaToken(ops, ctx, encoded) != 0) {
        return {hipObjS3Error, 0};
    }
    char   replyBuf[512];
    size_t replyLen   = sizeof(replyBuf);
    int    rdmaStatus = 0;
    if (receiveRdmaReplyRaw(ops, ctx, replyBuf, &replyLen, rdmaStatus) != 0 || rdmaStatus != 0) {
        return {hipObjS3Error, 0};
    }
    if (finishTransferAfterReply(replyBuf, replyLen, requiresDeviceSync) != 0) {
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
    if (!config) {
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
    int ret = (devName) ? hipObj::openRdmaDeviceByName(devName, hipObj::g_conn)
                        : hipObj::openRdmaDevice(nicIndex, hipObj::g_conn);
    if (ret != 0) {
        return {hipObjRdmaError, 0};
    }
    ret = hipObj::createRcQp(hipObj::g_conn, 256, 128, 128);
    if (ret != 0) {
        hipObj::closeRdmaDevice(hipObj::g_conn);
        return {hipObjRdmaError, 0};
    }
    ret = hipObj::transitionQpToInit(hipObj::g_conn);
    if (ret != 0) {
        hipObj::closeRdmaDevice(hipObj::g_conn);
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjSuccess, 0};
    }
#ifdef HIPOBJECT_V2_API
    std::lock_guard<std::mutex> apiGuard(hipObj::v2::apiLock());
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
    hipObj::g_bufferMap.deregisterAll();
    hipObj::closeRdmaDevice(hipObj::g_conn);
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (size > hipObj::MAX_MR_SIZE) {
        return {hipObjSizeTooLarge, 0};
    }
    if (hipObj::g_bufferMap.isRegistered(devPtr)) {
        return {hipObjBufAlreadyRegistered, 0};
    }
    int ret = hipObj::g_bufferMap.registerBuffer(devPtr, size, hipObj::g_conn.pd);
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!hostPtr) {
        return {hipObjInvalidValue, 0};
    }
    if (size > hipObj::MAX_MR_SIZE) {
        return {hipObjSizeTooLarge, 0};
    }
    if (hipObj::g_bufferMap.isRegistered(hostPtr)) {
        return {hipObjBufAlreadyRegistered, 0};
    }
    int ret = hipObj::g_bufferMap.registerHostBuffer(hostPtr, size, hipObj::g_conn.pd);
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
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!hipObj::g_bufferMap.isRegistered(devPtr)) {
        return {hipObjBufNotRegistered, 0};
    }
    int ret = hipObj::g_bufferMap.deregisterBuffer(devPtr);
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
    (void)handle;
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!ops) {
        return {hipObjInvalidValue, 0};
    }
    if (!hipObj::g_bufferMap.lookupMr(devPtr)) {
        return {hipObjBufNotRegistered, 0};
    }
    hipObjError_t err = hipObj::runRdmaTransfer(devPtr, size, offset, ops, ctx);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::stageBuffer(devPtr, size, offset, true);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjPut(hipObjHandle_t handle, const void *devPtr, size_t size, off_t offset, hipObjOps_t *ops, void *ctx)
try {
    (void)handle;
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!ops) {
        return {hipObjInvalidValue, 0};
    }
    if (!hipObj::g_bufferMap.lookupMr(const_cast<void *>(devPtr))) {
        return {hipObjBufNotRegistered, 0};
    }
    hipObjError_t serr = hipObj::stageBuffer(const_cast<void *>(devPtr), size, offset, false);
    if (serr.opError != hipObjSuccess) {
        return serr;
    }
    return hipObj::runRdmaTransfer(devPtr, size, offset, ops, ctx);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjBufSync(void *devPtr, size_t size, off_t offset, int direction)
try {
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!devPtr || (direction != HIPOBJ_SYNC_TO_HOST && direction != HIPOBJ_SYNC_TO_DEVICE)) {
        return {hipObjInvalidValue, 0};
    }
    if (!hipObj::g_bufferMap.isRegistered(devPtr)) {
        return {hipObjBufNotRegistered, 0};
    }
    return hipObj::stageBuffer(devPtr, size, offset, direction == HIPOBJ_SYNC_TO_DEVICE);
}
catch (...) {
    return hipObj::handleException();
}

hipObjError_t
hipObjGetRdmaToken(const void *devPtr, size_t size, int op, char **outToken)
try {
    hipObj::DriverState &state = hipObj::getState();
    if (!state.initialized) {
        return {hipObjNotInitialized, 0};
    }
    if (!devPtr || !outToken || size == 0) {
        return {hipObjInvalidValue, 0};
    }
    if (op != HIPOBJ_RDMA_OP_PUT && op != HIPOBJ_RDMA_OP_GET) {
        return {hipObjInvalidValue, 0};
    }
    if (!hipObj::g_bufferMap.lookupMr(const_cast<void *>(devPtr))) {
        return {hipObjBufNotRegistered, 0};
    }
    hipObj::RdmaToken token{};
    if (!hipObj::buildRdmaToken(devPtr, size, 0, token)) {
        return {hipObjRdmaError, 0};
    }
    std::string encoded = hipObj::encodeRdmaToken(token);
    char       *copy    = static_cast<char *>(std::malloc(encoded.size() + 1));
    if (!copy) {
        return {hipObjInternalError, 0};
    }
    std::memcpy(copy, encoded.c_str(), encoded.size() + 1);
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
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d.%d", HIPOBJ_VERSION_MAJOR, HIPOBJ_VERSION_MINOR, HIPOBJ_VERSION_PATCH);
    return buf;
}
catch (...) {
    return "0.0.0";
}

#ifdef HIPOBJECT_V2_API
// Not yet implemented — callers fall back to the v1 RDMA path.
hipObjError_t
hipObjPutV2(const char *, const char *, const void *, uint64_t, uint64_t, const char *, hipObjOpsV2_t *,
            void *)
{
    return {hipObjNotSupported, 0};
}

hipObjError_t
hipObjGetV2(const char *, const char *, void *, uint64_t, uint64_t, const char *, hipObjOpsV2_t *, void *)
{
    return {hipObjNotSupported, 0};
}
#endif /* HIPOBJECT_V2_API */

} // extern "C"
