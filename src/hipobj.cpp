/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "abandoned-copies.h"
#include "buffer.h"
#include "connection-pool.h"
#include "control.h"
#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj-print.h"
#include "hipobj-private.h"
#include "hipobj-span.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "log.h"
#include "rc-listener.h"
#include "state.h"
#include "token.h"
#include "transport.h"
#include "v3-wire.h"

namespace hipObj {

namespace {

    /* Returned by a public function when it catches an exception, whatever
     * its type */
    constexpr hipObjError_t kExceptionError = makeError(hipObjInternalError);

    /* The room a transfer gives recvReply for the x-amz-rdma-reply value.
     * The longest valid value, "200:" and a peer token, takes 92 bytes. */
    constexpr size_t kReplyBufLen = 512;

    /* A token that hipObjGetRdmaToken() hands to its caller, and
     * hipObjPutRdmaToken() takes back. The [] is std::unique_ptr's form for
     * an array allocated at run time, which std::array can't replace, not a
     * C array. */
    // NOLINTBEGIN(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
    using TokenPtr = std::unique_ptr<char[]>;

    /* Space for a token of size bytes. hipObjGetRdmaToken() fills all of
     * it, so it isn't zeroed. */
    TokenPtr allocateToken(size_t size)
    {
        return std::make_unique_for_overwrite<char[]>(size);
    }
    // NOLINTEND(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)

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

    /* What a transfer returns when it can't lease a connection */
    hipObjError_t acquireError(ConnectionPool::AcquireError err)
    {
        if (err == ConnectionPool::AcquireError::Closed) {
            /* The pool belongs to an instance that hipObjShutdown() has
             * removed */
            return makeError(hipObjNotInitialized);
        }
        if (err == ConnectionPool::AcquireError::Busy) {
            /* The transfer was started from another transfer's callback,
             * and every connection is leased */
            return makeError(hipObjBusy);
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

    [[nodiscard]] bool finishTransferAfterReply(const HipRuntime &runtime, ConnectionPool::Lease &lease,
                                                std::string_view reply, bool requiresDeviceSync,
                                                std::chrono::milliseconds pollTimeout)
    {
        if (const auto peerToken = parsePeerTokenFromReply(reply)) {
            if (!lease.connect(*peerToken)) {
                return false;
            }
        }
        // A poll timeout or error completion is a failure: the transfer outcome
        // is unknown. With the current one-sided protocol the responder side
        // may not observe a completion at all (see issue #22), so this check
        // reports "no evidence of failure" rather than "transfer verified".
        if (!pollCompletion(lease.connection(), std::nullopt, pollTimeout)) {
            return false;
        }
        if (!requiresDeviceSync) {
            return true;
        }
        return runtime.deviceSynchronize() == hipSuccess;
    }

    // hipobj-rc-v3 (see v3-wire.h): unlike v1's buildRdmaToken(), this
    // carries no QP number, GID or LID -- the peer learns those by
    // dialing the RC-CM listener (rc-listener.h) at the advertised
    // ip/port, and RDMA CM negotiates the QP-level connection itself.
    std::optional<v3::RdmaTokenV3> buildRdmaTokenV3(const Instance &instance, const BufferMap::BufEntry &buf,
                                                    size_t size, off_t offset)
    {
        if (!buf.mr) {
            return std::nullopt;
        }
        if (!rangeInRegistration(offset, size, buf.size)) {
            return std::nullopt;
        }
        /* hipObjInit() only succeeds with the listener running, but a token
         * without one would send the server nowhere */
        if (!instance.listener) {
            return std::nullopt;
        }
        v3::RdmaTokenV3 token;
        token.listenIp   = instance.listenIp;
        token.listenPort = instance.listener->port();
        token.rkey       = buf.mr->rkey;
        token.remoteAddr = buf.remoteAddr + static_cast<uint64_t>(offset);
        token.length     = static_cast<uint64_t>(size);
        /* Every transfer gets here, and connectionCount() takes the
         * listener's lock */
        if (logEnabled(LogLevel::Debug)) {
            logDebug("built RDMA token listenIp={} listenPort={} length={} "
                     "(rcListenerConnectionCount={})",
                     token.listenIp.empty() ? "(empty!)" : token.listenIp.c_str(), token.listenPort,
                     token.length, instance.listener->connectionCount());
        }
        return token;
    }

    /* hipMemcpy with a deadline (see Options::stageTimeout), between the
     * caller's memory and the staging buffer of buf. Async copy plus a
     * recorded event polled to a wall-clock bound. On timeout, the caller
     * gets an error instead of a hang, which is the whole point. The copy
     * may still land, and there's no safe way to stop it, so the event and
     * the registration are handed to the instance's abandoned copies,
     * which keep the staging buffer until the copy is over. Room for that
     * is set aside before the copy starts, so handing them over can't
     * fail and leave a running copy untracked. */
    hipObjError_t stageCopyWithDeadline(Instance                                         &instance,
                                        const std::shared_ptr<const BufferMap::BufEntry> &buf, void *dst,
                                        const void *src, size_t size, hipMemcpyKind kind)
    {
        AbandonedCopies::Slot slot    = instance.abandoned.reserve();
        const HipRuntime     &runtime = instance.platform->hip();
        hipEvent_t            rawDone = nullptr;
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
                instance.abandoned.add(slot, std::move(done), buf);
                const std::string_view direction =
                    kind == hipMemcpyHostToDevice ? "host-to-device" : "device-to-host";
                printTo(stderr,
                        "hipObj: staging {} copy of {} bytes did not complete within "
                        "{} ms; abandoning it. The GPU never signalled completion -- "
                        "see HIPOBJ_STAGE_TIMEOUT_MS.\n",
                        direction, size, timeout.count());
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

    hipObjError_t runRdmaTransferV3(const Instance &instance, const BufferMap::BufEntry &buf, size_t size,
                                    off_t offset, const hipObjOps_t &ops, void *ctx)
    {
        const auto token = buildRdmaTokenV3(instance, buf, size, offset);
        if (!token) {
            logError("buildRdmaTokenV3 failed (no MR registered for this pointer, "
                     "or offset+size out of range)");
            return makeError(hipObjRdmaError);
        }
        const v3::RdmaTokenV3Hex encodedToken = v3::encodeRdmaTokenV3Hex(*token);
        logDebug("encoded token ({} bytes)", v3::kRdmaTokenV3HexLen);
        if (!injectRdmaToken(ops, ctx,
                             asStringView(std::span(encodedToken).first<v3::kRdmaTokenV3HexLen>()))) {
            logError("injectRdmaToken (sendRequest) failed -- HTTP control request "
                     "itself did not complete");
            return makeError(hipObjS3Error);
        }
        const auto received = receiveRdmaReplyV3(ops, ctx);
        if (!received) {
            logError("receiveRdmaReplyV3 failed -- reply header missing or "
                     "unparsable");
            return makeError(hipObjS3Error);
        }
        const v3::RdmaReplyV3 &reply = *received;
        if (logEnabled(LogLevel::Debug)) {
            logDebug("reply httpStatus={} bytesTransferred={} "
                     "(rcListenerConnectionCount={})",
                     reply.httpStatus, reply.bytesTransferred,
                     instance.listener ? instance.listener->connectionCount() : 0);
        }
        // The peer establishes the RC connection out-of-band via the RC-CM
        // listener (or already has), and performs the one-sided RDMA
        // READ/WRITE itself; there is no local completion to poll for and
        // no peer QP info to feed into a manual RTR/RTS transition the way
        // v1's finishTransferAfterReply() needed. The HTTP reply status is
        // the only signal hipObject gets.
        /* OK, No Content and Partial Content: the statuses a successful
         * PUT or ranged GET returns */
        constexpr int kHttpOk             = 200;
        constexpr int kHttpNoContent      = 204;
        constexpr int kHttpPartialContent = 206;
        if (reply.httpStatus != kHttpOk && reply.httpStatus != kHttpNoContent &&
            reply.httpStatus != kHttpPartialContent) {
            return makeError(hipObjS3Error);
        }
        // The peer reports the bytes it moved. Anything but the whole range is a
        // failure: a GET left part of the buffer unwritten, or a PUT stored a
        // truncated object, and hipObjGet()/hipObjPut() can't report a partial
        // count.
        if (reply.bytesTransferred != size) {
            logError("the server transferred {} of {} bytes", reply.bytesTransferred, size);
            return makeError(hipObjS3Error);
        }
        // v1's finishTransferAfterReply() ran this same check after its local
        // completion poll; v3 has no local poll (see the comment above), but
        // the underlying reason a dmabuf-registered buffer needs a device
        // sync after the peer's one-sided RDMA op is unrelated to how
        // completion was observed -- it still applies here.
        if (buf.isDmabuf) {
            const hipError_t err = instance.platform->hip().deviceSynchronize();
            if (err != hipSuccess) {
                return makeError(hipObjRdmaError);
            }
        }
        return makeError(hipObjSuccess);
    }

    hipObjError_t runRdmaTransferV1(Instance &instance, const BufferMap::BufEntry &buf, size_t size,
                                    off_t offset, const hipObjOps_t &ops, void *ctx)
    {
        /* Waits for a connection if every one is leased, unless this thread
         * already holds one. The lease returns it when it's destroyed, after
         * the transfer, resetting it if it was connected. No lock is held
         * while the transfer runs, so the callbacks can call any public
         * function. */
        auto acquired = instance.pool.acquire();
        if (const auto *err = std::get_if<ConnectionPool::AcquireError>(&acquired)) {
            return acquireError(*err);
        }
        auto &lease = std::get<ConnectionPool::Lease>(acquired);

        const auto token = buildRdmaToken(lease.connection(), buf, size, offset);
        if (!token) {
            logError("buildRdmaToken failed (no MR registered for this pointer, no "
                     "QP, or offset+size out of range)");
            return makeError(hipObjRdmaError);
        }
        const RdmaTokenHex encoded = encodeRdmaTokenHex(*token);
        if (!injectRdmaToken(ops, ctx, asStringView(std::span(encoded).first<kRdmaTokenHexLen>()))) {
            logError("injectRdmaToken (sendRequest) failed -- HTTP control request "
                     "itself did not complete");
            return makeError(hipObjS3Error);
        }
        std::array<char, kReplyBufLen> replyBuf{};
        const auto                     reply = receiveRdmaReplyRaw(ops, ctx, replyBuf);
        if (!reply || reply->status != RdmaReplyStatus::Success) {
            logError("receiveRdmaReplyRaw failed -- reply header missing, "
                     "unparsable, or rdmaStatus={}",
                     reply ? static_cast<int>(reply->status) : -1);
            return makeError(hipObjS3Error);
        }
        if (!finishTransferAfterReply(instance.platform->hip(), lease, reply->text, buf.isDmabuf,
                                      instance.options.pollTimeout)) {
            return makeError(hipObjRdmaError);
        }
        return makeError(hipObjSuccess);
    }

    hipObjError_t runRdmaTransfer(Instance &instance, const BufferMap::BufEntry &buf, size_t size,
                                  off_t offset, const hipObjOps_t &ops, void *ctx)
    {
        return useV1Wire() ? runRdmaTransferV1(instance, buf, size, offset, ops, ctx)
                           : runRdmaTransferV3(instance, buf, size, offset, ops, ctx);
    }

    /* The PD to register buffers against. In v3, the RC-CM listener's own PD:
     * an accepted connection's QP is on it, and a QP can only reach MRs in its
     * own PD. The listener owns that PD and frees it when it stops, after
     * the instance's buffers are deregistered. In v1, the device's. */
    ibv_pd *registrationPd(const Instance &instance)
    {
        return instance.listener ? instance.listener->pd() : instance.device->pd();
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
    hipObj::shutdownInstance();
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjBufRegister(void *devPtr, size_t size)
try {
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
    return hipObj::registrationError(
        instance->buffers.registerBuffer(devPtr, size, hipObj::registrationPd(*instance)));
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjBufRegisterHost(void *hostPtr, size_t size)
try {
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    if (!instance) {
        return hipObj::makeError(hipObjNotInitialized);
    }
    const hipObjError_t err = hipObj::checkRegistrationArgs(hostPtr, size);
    if (err.opError != hipObjSuccess) {
        return err;
    }
    return hipObj::registrationError(
        instance->buffers.registerHostBuffer(hostPtr, size, hipObj::registrationPd(*instance)));
}
catch (...) {
    return hipObj::kExceptionError;
}

hipObjError_t
hipObjBufDeregister(void *devPtr)
try {
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
    /* Busy: another call, such as a transfer, or a staging copy that timed
     * out, is still using the buffer */
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
    hipObj::RdmaTokenHex       v1Hex{};
    hipObj::v3::RdmaTokenV3Hex v3Hex{};
    std::string_view           encoded;
    if (hipObj::useV1Wire()) {
        /* A v1 instance always has one (see initWith()) */
        if (!instance->tokenConn) {
            return hipObj::makeError(hipObjInternalError);
        }
        const auto token = hipObj::buildRdmaToken(*instance->tokenConn, *buf, size, 0);
        if (!token) {
            return hipObj::makeError(hipObjRdmaError);
        }
        v1Hex   = hipObj::encodeRdmaTokenHex(*token);
        encoded = hipObj::asStringView(std::span(v1Hex));
    }
    else {
        const auto token = hipObj::buildRdmaTokenV3(*instance, *buf, size, 0);
        if (!token) {
            return hipObj::makeError(hipObjRdmaError);
        }
        v3Hex   = hipObj::v3::encodeRdmaTokenV3Hex(*token);
        encoded = hipObj::asStringView(std::span(v3Hex));
    }
    hipObj::TokenPtr copy = hipObj::allocateToken(encoded.size());
    std::ranges::copy(encoded, hipObj::spanFromPointer(copy.get(), encoded.size()).begin());
    *outToken = copy.release();
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
    /* From hipObjGetRdmaToken() */
    const hipObj::TokenPtr owned(token);
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
hipObjParseRdmaReplyV3(const char *reply, size_t replyLen, int *httpCode, uint64_t *bytesTransferred)
try {
    if (reply == nullptr || httpCode == nullptr || bytesTransferred == nullptr) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    const auto parsed =
        hipObj::v3::decodeRdmaReplyV3(hipObj::asStringView(hipObj::spanFromPointer(reply, replyLen)));
    if (!parsed) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    *httpCode         = parsed->httpStatus;
    *bytesTransferred = parsed->bytesTransferred;
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
    if (hipObj::useV1Wire()) {
        /* Bound the scan: anything longer than a token is rejected anyway */
        if (!hipObj::parseClientNicFromTokenHex(hipObj::cStringView(token, hipObj::kRdmaTokenHexLen + 1),
                                                hipObj::spanFromPointer(nicIp, nicIpLen))) {
            return hipObj::makeError(hipObjInvalidValue);
        }
        return hipObj::makeError(hipObjSuccess);
    }
    /* A v3 token names the address the server dials, the RC-CM listener's
     * on the client's NIC. Bound the scan: anything longer than a token is
     * rejected anyway. */
    const std::span<char> out = hipObj::spanFromPointer(nicIp, nicIpLen);
    out[0]                    = '\0';
    const auto decoded =
        hipObj::v3::decodeRdmaTokenV3(hipObj::cStringView(token, hipObj::v3::kRdmaTokenV3HexLen + 1));
    if (!decoded || decoded->listenIp.size() >= nicIpLen) {
        return hipObj::makeError(hipObjInvalidValue);
    }
    std::ranges::copy(decoded->listenIp, out.begin());
    out[decoded->listenIp.size()] = '\0';
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
}

const char *
hipObjGetVersionString(void)
try {
    /* Formatted once: C++ runs a function-local static's initializer
     * exactly once, even when several threads call this at the same time.
     * It's an array rather than a std::string so that it has nothing to
     * destroy when the program exits. format_to_n() doesn't write a NUL, so
     * it gets one less than the array's size, which leaves the last
     * element's zero to end the string. */
    static const std::array<char, 32> version = [] {
        std::array<char, 32> buf{};
        std::format_to_n(buf.begin(), buf.size() - 1, "{}.{}.{}", HIPOBJ_VERSION_MAJOR, HIPOBJ_VERSION_MINOR,
                         HIPOBJ_VERSION_PATCH);
        return buf;
    }();
    return version.data();
}
catch (...) {
    return "0.0.0";
}

hipObjError_t
hipObjGetVersion(unsigned *major, unsigned *minor, unsigned *patch)
try {
    /* NULL skips that part */
    if (major != nullptr) {
        *major = HIPOBJ_VERSION_MAJOR;
    }
    if (minor != nullptr) {
        *minor = HIPOBJ_VERSION_MINOR;
    }
    if (patch != nullptr) {
        *patch = HIPOBJ_VERSION_PATCH;
    }
    return hipObj::makeError(hipObjSuccess);
}
catch (...) {
    return hipObj::kExceptionError;
}

} // extern "C"
