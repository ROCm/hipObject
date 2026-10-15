/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"

namespace hipObj {

namespace {

    [[nodiscard]] bool validateRegistration(bool isRegistered, size_t entryCount, size_t size)
    {
        return size <= MAX_MR_SIZE && !isRegistered && entryCount < BufferMap::kMaxEntries;
    }

    /* Is this pointer device memory? Answered through the seam so unit tests can
     * drive both branches without a GPU. A pointer the runtime cannot classify is
     * treated as "not device memory": the strict check below must not fail a host
     * buffer it merely failed to classify. */
    bool isDevicePointer(const HipRuntime &runtime, void *ptr)
    {
        hipPointerAttribute_t attr{};
        if (runtime.pointerGetAttributes(&attr, ptr) != hipSuccess) {
            return false;
        }
        return attr.type == hipMemoryTypeDevice;
    }

    /* Owns a dmabuf fd from HipRuntime::exportDmabuf(), and closes it
     * with the same runtime's closeDmabuf(). Only the registration needs
     * it: ibv_reg_dmabuf_mr() takes its own reference to the dma-buf,
     * which the MR keeps until it's deregistered, so the fd is closed as
     * soon as the registration call returns, whether or not it
     * succeeded. */
    class DmabufFd {
    public:
        DmabufFd(int fd, const HipRuntime &runtime) noexcept : fd_(fd), runtime_(&runtime)
        {
        }
        ~DmabufFd()
        {
            (void)runtime_->closeDmabuf(fd_);
        }

        DmabufFd(const DmabufFd &)            = delete;
        DmabufFd &operator=(const DmabufFd &) = delete;
        DmabufFd(DmabufFd &&)                 = delete;
        DmabufFd &operator=(DmabufFd &&)      = delete;

        [[nodiscard]] int get() const noexcept
        {
            return fd_;
        }

    private:
        int               fd_;
        const HipRuntime *runtime_;
    };

} // namespace

IbvMrPtr
registerDeviceMemory(const Verbs &verbs, const HipRuntime &runtime, struct ibv_pd *pd, void *addr,
                     size_t length, int access, const DmabufPolicy &policy)
{
    /* Whether a given GPU can export a dma-buf and a given NIC can register
     * it is answered by trying: both calls report their own failure and
     * fall through. This used to additionally require
     * CONFIG_DMABUF_MOVE_NOTIFY=y and CONFIG_PCI_P2PDMA=y from
     * /boot/config-$(uname -r), which is a false negative on any kernel
     * that registers dmabuf MRs without move-notify -- the mainline builds
     * the emulated-hardware CI lanes run on do exactly that, and lost
     * GPU-direct to a grep. A config file is a prediction; the
     * registration call is the fact. */
    if (policy.enabled && verbs.supportsDmabuf()) {
        uint64_t offset = 0;
        int      fd     = -1;

        const int status = runtime.exportDmabuf(addr, length, &fd, &offset);
        if (status != 0) {
            /* Not an error worth a line on stderr by default: this is also the
             * answer for a host pointer, and the host-buffer paths register far
             * more often than the device ones. The plain path below handles it. */
            if (policy.debug) {
                fprintf(stderr, "hipObj: hsa_amd_portable_export_dmabuf failed: %d\n", status);
            }
        }
        else {
            const DmabufFd dmabuf(fd, runtime);
            IbvMrPtr       mr = own<IbvMrPtr>(verbs, verbs.reg_dmabuf_mr(pd, offset, length,
                                                                         reinterpret_cast<uintptr_t>(addr),
                                                                         dmabuf.get(), access));
            if (mr) {
                return mr;
            }
            const std::string reason = std::generic_category().message(errno);
            fprintf(stderr, "hipObj: ibv_reg_dmabuf_mr failed: %s\n", reason.c_str());
        }
        // Fall through to the plain path: it fails too for device memory, but
        // the caller distinguishes "no MR" from "wrong MR" and we must not
        // invent a registration here.
    }

    return own<IbvMrPtr>(verbs, verbs.reg_mr(pd, addr, length, access));
}

bool
isValidDeviceRange(const HipRuntime &runtime, void *ptr, size_t size)
{
    if (!isDevicePointer(runtime, ptr)) {
        return true;
    }
    hipDeviceptr_t base      = nullptr;
    size_t         allocSize = 0;
    if (runtime.memGetAddressRange(&base, &allocSize, ptr) != hipSuccess) {
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
    (void)runtime->hostFree(hostBuf);
}

bool
BufferMap::registerBuffer(void *devPtr, size_t size, struct ibv_pd *pd)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    if (!validateRegistration(entries_.contains(key), entries_.size(), size)) {
        return false;
    }
    const Verbs      &verbs   = *verbs_;
    const HipRuntime &runtime = *runtime_;
    int               access  = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    IbvMrPtr          mr      = registerDeviceMemory(verbs, runtime, pd, devPtr, size, access, dmabuf_);
    if (mr) {
        /* The NIC reaches the caller's memory, so there's no staging
         * buffer */
        entries_.try_emplace(key, own<HostBufPtr>(runtime, nullptr), std::move(mr), size, true, key);
        return true;
    }

    const bool deviceMemory = isDevicePointer(runtime, devPtr);
    if (deviceMemory) {
        if (requireGpuDirect_) {
            fprintf(stderr,
                    "hipObj: GPU-direct registration of %zu bytes at %p failed and "
                    "HIPOBJ_REQUIRE_GPU_DIRECT is set; refusing to stage through "
                    "host memory.\n",
                    size, devPtr);
            return false;
        }
        fprintf(stderr,
                "hipObj: GPU-direct registration of %zu bytes at %p failed; "
                "falling back to a host staging buffer. This transfer is no "
                "longer GPU-direct.\n",
                size, devPtr);
    }

    void      *rawHostBuf = nullptr;
    hipError_t err        = runtime.hostMalloc(&rawHostBuf, size, hipHostMallocDefault);
    if (err != hipSuccess || !rawHostBuf) {
        return false;
    }
    HostBufPtr hostBuf = own<HostBufPtr>(runtime, rawHostBuf);
    mr                 = own<IbvMrPtr>(verbs, verbs.reg_mr(pd, hostBuf.get(), size, access));
    if (!mr) {
        return false;
    }
    /* Read the address before hostBuf is moved into the entry */
    const auto remoteAddr = reinterpret_cast<uint64_t>(hostBuf.get());
    /* Build the entry before inserting it. mr is declared before
     * hostBuf, so if allocating the map node throws, unwinding would
     * free the staging buffer while it's still registered; the entry
     * deregisters its MR first. */
    BufEntry entry(std::move(hostBuf), std::move(mr), size, false, remoteAddr);
    entries_.try_emplace(key, std::move(entry));
    return true;
}

bool
BufferMap::registerHostBuffer(void *hostPtr, size_t size, struct ibv_pd *pd)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(hostPtr);
    if (!validateRegistration(entries_.contains(key), entries_.size(), size)) {
        return false;
    }
    const Verbs &verbs  = *verbs_;
    int          access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    IbvMrPtr     mr     = own<IbvMrPtr>(verbs, verbs.reg_mr(pd, hostPtr, size, access));
    if (!mr) {
        return false;
    }
    entries_.try_emplace(key, own<HostBufPtr>(*runtime_, nullptr), std::move(mr), size, false,
                         reinterpret_cast<uint64_t>(hostPtr));
    return true;
}

bool
BufferMap::deregisterBuffer(void *devPtr)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
    auto      it  = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    /* Deregisters the MR, then frees any staging buffer */
    entries_.erase(it);
    return true;
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

size_t
BufferMap::size() const
{
    return entries_.size();
}
} // namespace hipObj
