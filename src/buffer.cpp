/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"

namespace hipObj {

namespace {

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

bool
isValidDeviceRange(void *ptr, size_t size)
{
    if (!isDevicePointer(ptr)) {
        return true;
    }
    HipOps &ops = hipOps();
    if (!ops.hipMemGetAddressRange) {
        return false;
    }
    hipDeviceptr_t base      = nullptr;
    size_t         allocSize = 0;
    if (ops.hipMemGetAddressRange(&base, &allocSize, ptr) != hipSuccess) {
        return false;
    }
    auto start = reinterpret_cast<uintptr_t>(base);
    auto addr  = reinterpret_cast<uintptr_t>(ptr);
    if (addr < start || addr - start >= allocSize) {
        return false;
    }
    /* Compared against the space left rather than as addr + size, which
     * could wrap */
    return size <= allocSize - (addr - start);
}

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
        BufEntry &entry  = entries_[key];
        entry.mr         = std::move(mr);
        entry.size       = size;
        entry.isDmabuf   = true;
        entry.remoteAddr = key;
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
    BufEntry &entry = entries_[key];
    /* Read the address before hostBuf is moved into the entry */
    entry.remoteAddr = reinterpret_cast<uint64_t>(hostBuf.get());
    entry.hostBuf    = std::move(hostBuf);
    entry.mr         = std::move(mr);
    entry.size       = size;
    entry.isDmabuf   = false;
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
    BufEntry &entry  = entries_[key];
    entry.mr         = std::move(mr);
    entry.size       = size;
    entry.isDmabuf   = false;
    entry.remoteAddr = reinterpret_cast<uint64_t>(hostPtr);
    return 0;
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

const BufferMap::BufEntry *
BufferMap::find(const void *devPtr) const
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

bool
BufferMap::isRegistered(const void *devPtr) const
{
    return find(devPtr) != nullptr;
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
