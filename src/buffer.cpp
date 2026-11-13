/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj-print.h"
#include "hipobj-private.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "thread-safety.h"

namespace hipObj {

namespace {

    /* Is this pointer device memory? Answered through the seam so unit tests can
     * drive both branches without a GPU. A pointer the runtime cannot classify is
     * treated as "not device memory": the strict check below must not fail a host
     * buffer it merely failed to classify. */
    bool isDevicePointer(const HipRuntime &runtime, const void *ptr)
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
registerDeviceMemory(const Verbs &verbs, const HipRuntime &runtime, ibv_pd *pd, void *addr, size_t length,
                     int access, const DmabufPolicy &policy)
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
                printTo(stderr, "hipObj: hsa_amd_portable_export_dmabuf failed: {}\n", status);
            }
        }
        else {
            const DmabufFd dmabuf(fd, runtime);
            auto           mr = own<IbvMrPtr>(
                verbs, verbs.reg_dmabuf_mr(pd, offset, length, pointerToAddress(addr), dmabuf.get(), access));
            if (mr) {
                return mr;
            }
            const std::string reason = std::generic_category().message(errno);
            printTo(stderr, "hipObj: ibv_reg_dmabuf_mr failed: {}\n", reason);
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
    const auto start = pointerToAddress(base);
    const auto addr  = pointerToAddress(ptr);
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
    (void)runtime_->hostFree(hostBuf);
}

std::optional<BufferMap::RegisterResult>
BufferMap::refusal(uintptr_t key) const
{
    if (entries_.contains(key)) {
        return RegisterResult::AlreadyRegistered;
    }
    if (entries_.size() >= kMaxEntries) {
        return RegisterResult::Failed;
    }
    return std::nullopt;
}

std::optional<BufferMap::RegisterResult>
BufferMap::precheck(uintptr_t key) const
{
    const SharedLock lock(mutex_);
    return refusal(key);
}

/* entry is a parameter, so it's destroyed after the lock, a local, is
 * released */
BufferMap::RegisterResult
BufferMap::insert(uintptr_t key, std::shared_ptr<const BufEntry> entry)
{
    const ScopedLock<SharedMutex> lock(mutex_);
    if (const auto refused = refusal(key)) {
        return *refused;
    }
    /* Assigned rather than passed to try_emplace(), since clang-tidy
     * doesn't see that try_emplace() moves from it */
    entries_[key] = std::move(entry);
    return RegisterResult::Registered;
}

BufferMap::RegisterResult
BufferMap::registerBuffer(void *devPtr, size_t size, ibv_pd *pd)
{
    if (size > MAX_MR_SIZE) {
        return RegisterResult::Failed;
    }
    const auto key = pointerToAddress(devPtr);
    if (const auto refused = precheck(key)) {
        return *refused;
    }
    const Verbs      &verbs   = *verbs_;
    const HipRuntime &runtime = *runtime_;
    const int         access  = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    IbvMrPtr          mr      = registerDeviceMemory(verbs, runtime, pd, devPtr, size, access, dmabuf_);
    if (mr) {
        /* The NIC reaches the caller's memory, so there's no staging
         * buffer */
        return insert(key, std::make_shared<const BufEntry>(own<HostBufPtr>(runtime, nullptr), std::move(mr),
                                                            size, true, key));
    }

    const bool deviceMemory = isDevicePointer(runtime, devPtr);
    if (deviceMemory) {
        if (requireGpuDirect_) {
            printTo(stderr,
                    "hipObj: GPU-direct registration of {} bytes at {} failed and "
                    "HIPOBJ_REQUIRE_GPU_DIRECT is set; refusing to stage through "
                    "host memory.\n",
                    size, devPtr);
            return RegisterResult::Failed;
        }
        printTo(stderr,
                "hipObj: GPU-direct registration of {} bytes at {} failed; "
                "falling back to a host staging buffer. This transfer is no "
                "longer GPU-direct.\n",
                size, devPtr);
    }

    void            *rawHostBuf = nullptr;
    const hipError_t err        = runtime.hostMalloc(&rawHostBuf, size, hipHostMallocDefault);
    if (err != hipSuccess || rawHostBuf == nullptr) {
        return RegisterResult::Failed;
    }
    auto hostBuf = own<HostBufPtr>(runtime, rawHostBuf);
    /* Declared after hostBuf, so if building the entry throws, the MR is
     * deregistered before the staging buffer it covers is freed */
    auto stagingMr = own<IbvMrPtr>(verbs, verbs.reg_mr(pd, hostBuf.get(), size, access));
    if (!stagingMr) {
        return RegisterResult::Failed;
    }
    /* Read the address before hostBuf is moved into the entry */
    const uint64_t remoteAddr = pointerToAddress(hostBuf.get());
    return insert(key, std::make_shared<const BufEntry>(std::move(hostBuf), std::move(stagingMr), size, false,
                                                        remoteAddr));
}

BufferMap::RegisterResult
BufferMap::registerHostBuffer(void *hostPtr, size_t size, ibv_pd *pd)
{
    if (size > MAX_MR_SIZE) {
        return RegisterResult::Failed;
    }
    const auto key = pointerToAddress(hostPtr);
    if (const auto refused = precheck(key)) {
        return *refused;
    }
    const Verbs &verbs  = *verbs_;
    const int    access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    auto         mr     = own<IbvMrPtr>(verbs, verbs.reg_mr(pd, hostPtr, size, access));
    if (!mr) {
        return RegisterResult::Failed;
    }
    return insert(key, std::make_shared<const BufEntry>(own<HostBufPtr>(*runtime_, nullptr), std::move(mr),
                                                        size, false, pointerToAddress(hostPtr)));
}

BufferMap::DeregisterResult
BufferMap::deregisterBuffer(void *devPtr)
{
    const auto                      key = pointerToAddress(devPtr);
    std::shared_ptr<const BufEntry> entry;
    {
        const ScopedLock<SharedMutex> lock(mutex_);
        const auto                    it = entries_.find(key);
        if (it == entries_.end()) {
            return DeregisterResult::NotRegistered;
        }
        /* A copy can only be made from one that's already held. find()
         * copies the map's own under the lock this holds exclusively,
         * and the other copies, such as the one AbandonedCopies::add()
         * keeps, are made from one that find() returned. So while this
         * holds the lock, a count of 1, the map's own copy, can't go
         * up. A copy that's released while this checks can only make
         * the entry look busy when it's not, never the reverse. */
        if (it->second.use_count() > 1) {
            return DeregisterResult::Busy;
        }
        entry = std::move(it->second);
        entries_.erase(it);
    }

    /* Deregisters the MR, then frees any staging buffer, without holding
     * the lock */
    entry.reset();
    return DeregisterResult::Deregistered;
}

std::shared_ptr<const BufferMap::BufEntry>
BufferMap::find(const void *devPtr) const
{
    const auto       key = pointerToAddress(devPtr);
    const SharedLock lock(mutex_);
    const auto       it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second;
}

size_t
BufferMap::size() const
{
    const SharedLock lock(mutex_);
    return entries_.size();
}
} // namespace hipObj
