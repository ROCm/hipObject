/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include <hip/hip_runtime.h>

#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-wrapper.h"

namespace hipObj {

namespace {

    constexpr int IBV_ACCESS_REMOTE_READ  = 0x1;
    constexpr int IBV_ACCESS_REMOTE_WRITE = 0x2;
    constexpr int IBV_ACCESS_LOCAL_WRITE  = 0x4;

    int validateRegistration(bool isRegistered, size_t entryCount, size_t size)
    {
        if (size > MAX_MR_SIZE) {
            return -1;
        }
        if (isRegistered) {
            return -1;
        }
        if (entryCount >= BufferMap::kMaxEntries) {
            return -1;
        }
        return 0;
    }

    /* Is this pointer device memory? Answered through the seam so unit tests can
     * drive both branches without a GPU. A runtime that cannot tell us is treated
     * as "not device memory": the strict check below must not fail a host buffer
     * it merely failed to classify. */
    bool isDevicePointer(void *ptr)
    {
        HipOps &ops = hipOps();
        if (!ops.hipPointerGetAttributes) {
            return false;
        }
        hipPointerAttribute_t attr{};
        if (ops.hipPointerGetAttributes(&attr, ptr) != hipSuccess) {
            return false;
        }
        return attr.type == hipMemoryTypeDevice;
    }

    /* Strict GPU-direct mode. Registering a device buffer is supposed to hand the
     * NIC the device memory itself; when that fails we silently fall back to a
     * host staging buffer and copy through it, which still transfers the right
     * bytes and so passes every assertion a test can make -- a GPU-direct lane
     * that quietly stopped being GPU-direct reports success. CI sets this so the
     * fallback is a hard failure there, while a production host without dmabuf
     * keeps working. */
    bool requireGpuDirect()
    {
        static bool required = [] {
            const char *env = getenv("HIPOBJ_REQUIRE_GPU_DIRECT");
            return env && *env && env[0] != '0' && env[0] != 'n' && env[0] != 'N';
        }();
        return required;
    }

} // namespace

void
HostBufDeleter::operator()(void *hostBuf) const noexcept
{
    auto freeFn = hipObj::hipOps().hipHostFree;
    if (freeFn) {
        (void)freeFn(hostBuf);
        return;
    }
    (void)hipHostFree(hostBuf);
}

int
BufferMap::registerBuffer(void *devPtr, size_t size, struct ibv_pd *pd)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    if (validateRegistration(entries_.find(key) != entries_.end(), entries_.size(), size) != 0) {
        return -1;
    }
    int      access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    IbvMrPtr mr(ibv.reg_mr(pd, devPtr, size, access));
    if (mr) {
        entries_[key] = BufEntry{.mr = std::move(mr), .size = size, .isDmabuf = true, .remoteAddr = key};
        return 0;
    }

    const bool deviceMemory = isDevicePointer(devPtr);
    if (deviceMemory) {
        if (requireGpuDirect()) {
            fprintf(stderr,
                    "hipObj: GPU-direct registration of %zu bytes at %p failed and "
                    "HIPOBJ_REQUIRE_GPU_DIRECT is set; refusing to stage through "
                    "host memory.\n",
                    size, devPtr);
            return -1;
        }
        fprintf(stderr,
                "hipObj: GPU-direct registration of %zu bytes at %p failed; "
                "falling back to a host staging buffer. This transfer is no "
                "longer GPU-direct.\n",
                size, devPtr);
    }

    void      *rawHostBuf = nullptr;
    hipError_t err        = hipObj::hipOps().hipHostMalloc(&rawHostBuf, size, hipHostMallocDefault);
    if (err != hipSuccess || !rawHostBuf) {
        return -1;
    }
    HostBufPtr hostBuf(rawHostBuf);
    mr.reset(ibv.reg_mr_host(pd, hostBuf.get(), size, access));
    if (!mr) {
        return -1;
    }
    /* Read the address before hostBuf is moved into the entry */
    auto remoteAddr = reinterpret_cast<uint64_t>(hostBuf.get());
    entries_[key]   = BufEntry{.hostBuf    = std::move(hostBuf),
                               .mr         = std::move(mr),
                               .size       = size,
                               .isDmabuf   = false,
                               .remoteAddr = remoteAddr};
    return 0;
}

int
BufferMap::registerHostBuffer(void *hostPtr, size_t size, struct ibv_pd *pd)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(hostPtr);
    if (validateRegistration(entries_.find(key) != entries_.end(), entries_.size(), size) != 0) {
        return -1;
    }
    int      access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    IbvMrPtr mr(ibv.reg_mr_host(pd, hostPtr, size, access));
    if (!mr) {
        return -1;
    }
    entries_[key] = BufEntry{.mr         = std::move(mr),
                             .size       = size,
                             .isDmabuf   = false,
                             .remoteAddr = reinterpret_cast<uint64_t>(hostPtr)};
    return 0;
}
uint64_t
BufferMap::lookupRemoteAddr(void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.remoteAddr;
}

int
BufferMap::deregisterBuffer(void *devPtr)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    if (it == entries_.end()) {
        return -1;
    }
    if (it->second.refCount > 0) {
        return -1; /* pinned by a live v2 connection */
    }
    /* Deregisters the MR, then frees any staging buffer */
    entries_.erase(it);
    return 0;
}

void
BufferMap::deregisterAll()
{
    entries_.clear();
}

struct ibv_mr *
BufferMap::lookupMr(void *devPtr)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    if (it == entries_.end()) {
        return nullptr;
    }
    return it->second.mr.get();
}

void *
BufferMap::lookupHostBuf(void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second.hostBuf.get();
}

size_t
BufferMap::lookupSize(void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    if (it == entries_.end()) {
        return 0;
    }
    return it->second.size;
}

bool
BufferMap::isRegistered(void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    return entries_.find(key) != entries_.end();
}

bool
BufferMap::requiresDeviceSync(void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    return it != entries_.end() && it->second.isDmabuf;
}

#ifdef HIPOBJECT_V2_API
bool
BufferMap::acquireMrRef(void *devPtr)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    ++it->second.refCount;
    return true;
}

bool
BufferMap::releaseMrRef(void *devPtr)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    if (it == entries_.end() || it->second.refCount == 0) {
        return false;
    }
    --it->second.refCount;
    return true;
}

size_t
BufferMap::mrRefCount(void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.refCount;
}

bool
BufferMap::anyPinned() const
{
    for (const auto &[key, ent] : entries_) {
        if (ent.refCount > 0) {
            return true;
        }
    }
    return false;
}
#endif /* HIPOBJECT_V2_API */

size_t
BufferMap::size() const
{
    return entries_.size();
}
} // namespace hipObj
