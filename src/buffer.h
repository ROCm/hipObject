/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Buffer registration and MR cache */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "ibv-core.h"
#include "ibv-ptr.h"
#include "thread-safety.h"

namespace hipObj {

class HipRuntime;
class Verbs;

/* How registerDeviceMemory() registers memory: whether it tries a
 * dma-buf registration first (HIPOBJ_DMABUF), and whether it reports an
 * export that fails (HIPOBJ_RDMA_DEBUG) */
struct DmabufPolicy {
    bool enabled = true;
    bool debug   = false;
};

/* Registers [addr, addr + length) with the NIC, for memory that may be
 * on a GPU. When policy and verbs allow it, it exports the memory from
 * runtime as a dma-buf and registers that, which is how the NIC reaches
 * GPU memory. Otherwise, or when either step fails, it registers the
 * memory as host memory, which fails for GPU memory. Null when nothing
 * registers. */
IbvMrPtr registerDeviceMemory(const Verbs &verbs, const HipRuntime &runtime, struct ibv_pd *pd, void *addr,
                              size_t length, int access, const DmabufPolicy &policy);

/* Frees a host staging buffer through the runtime whose hostMalloc()
 * allocated it, which has to outlive the pointer. Create the pointer with
 * own() (ibv-ptr.h). */
class HostBufDeleter {
public:
    explicit HostBufDeleter(const HipRuntime &deleterRuntime) noexcept : runtime_(&deleterRuntime)
    {
    }
    void operator()(void *hostBuf) const noexcept;

private:
    const HipRuntime *runtime_;
};

using HostBufPtr = std::unique_ptr<void, HostBufDeleter>;

/* False when runtime reports ptr as device memory and [ptr, ptr + size)
 * doesn't fit inside the allocation that holds ptr, or when that
 * allocation can't be found. Memory the runtime doesn't report as device
 * memory passes: registering it with the NIC checks that it's mapped. */
bool isValidDeviceRange(const HipRuntime &runtime, void *ptr, size_t size);

/* The registered buffers, by address. Several threads can use it at once.
 *
 * The map holds each entry in a shared_ptr, and find() returns a copy,
 * which keeps the entry alive, without holding the map's lock, for as
 * long as the caller needs it. deregisterBuffer() refuses to remove an
 * entry while anything else holds a copy, so a buffer can't be
 * deregistered while a transfer is using it. */
class BufferMap {
public:
    static constexpr size_t kMaxEntries = 256;

    /* hostBuf is declared before mr so the MR is deregistered before
     * the staging buffer it covers is freed. Lives in a shared_ptr, and
     * isn't copied or moved. */
    struct BufEntry {
        /* Host staging buffer, or null when the NIC reaches the caller's
         * memory directly */
        HostBufPtr hostBuf;
        IbvMrPtr   mr;
        size_t     size;
        /* The NIC reads and writes the caller's device memory, so a
         * transfer has to synchronize with the device */
        bool isDmabuf;
        /* Address to advertise to the peer. Not mr->addr:
         * ibv_reg_dmabuf_mr leaves that NULL, and on the bounce path it
         * is the host staging buffer rather than the caller's pointer. */
        uint64_t remoteAddr;
        /* Set when a staging copy that timed out may still use hostBuf
         * when the entry is destroyed (see AbandonedCopies), so the
         * staging buffer is leaked rather than freed. It's mutable since
         * the entry is otherwise const once it's built. */
        mutable std::atomic<bool> leakHostBuf{false};

        BufEntry(HostBufPtr entryHostBuf, IbvMrPtr entryMr, size_t entrySize, bool entryIsDmabuf,
                 uint64_t entryRemoteAddr) noexcept
            : hostBuf(std::move(entryHostBuf)), mr(std::move(entryMr)), size(entrySize),
              isDmabuf(entryIsDmabuf), remoteAddr(entryRemoteAddr)
        {
        }
        /* The members are destroyed after this runs, so the MR is still
         * deregistered */
        ~BufEntry()
        {
            if (leakHostBuf.load()) {
                dropOwnership(hostBuf);
            }
        }
        BufEntry(const BufEntry &)            = delete;
        BufEntry &operator=(const BufEntry &) = delete;
        BufEntry(BufEntry &&)                 = delete;
        BufEntry &operator=(BufEntry &&)      = delete;
    };

    /* What registerBuffer() and registerHostBuffer() did. Failed covers
     * an oversize buffer, a full map, and a registration that failed. */
    enum class RegisterResult { Registered, AlreadyRegistered, Failed };

    /* What deregisterBuffer() did. It's Busy when something else holds the
     * entry (see find()). */
    enum class DeregisterResult { Deregistered, NotRegistered, Busy };

    /* Registers memory through verbs, and allocates host staging buffers
     * from runtime, which both have to outlive it. registerBuffer()
     * registers GPU memory as dmabuf says, and with requireGpuDirect, fails
     * rather than staging a GPU buffer the NIC can't reach through host
     * memory. */
    BufferMap(const Verbs &verbs, const HipRuntime &runtime, const DmabufPolicy &dmabuf,
              bool requireGpuDirect) noexcept
        : verbs_(&verbs), runtime_(&runtime), dmabuf_(dmabuf), requireGpuDirect_(requireGpuDirect)
    {
    }
    ~BufferMap() = default;

    /* Lives in one place, and isn't copied or moved */
    BufferMap(const BufferMap &)            = delete;
    BufferMap &operator=(const BufferMap &) = delete;
    BufferMap(BufferMap &&)                 = delete;
    BufferMap &operator=(BufferMap &&)      = delete;

    /* Register with the NIC, and allocate any staging buffer, without
     * holding the map's lock. When two threads register the same address
     * at once, one gets Registered, and the other AlreadyRegistered, and
     * its registration is released. */
    [[nodiscard]] RegisterResult registerBuffer(void *devPtr, size_t size, struct ibv_pd *pd)
        HIPOBJ_REQUIRES(!mutex_);
    [[nodiscard]] RegisterResult registerHostBuffer(void *hostPtr, size_t size, struct ibv_pd *pd)
        HIPOBJ_REQUIRES(!mutex_);
    /* Removes the entry, and releases its registration and any staging
     * buffer without holding the map's lock */
    [[nodiscard]] DeregisterResult deregisterBuffer(void *devPtr) HIPOBJ_REQUIRES(!mutex_);
    /* The registration at devPtr, or null if there isn't one. Looked up
     * once per transfer and passed down. The copy keeps the entry alive,
     * and makes deregisterBuffer() report Busy, until it's released. */
    [[nodiscard]] std::shared_ptr<const BufEntry> find(const void *devPtr) const HIPOBJ_REQUIRES(!mutex_);

    [[nodiscard]] size_t size() const HIPOBJ_REQUIRES(!mutex_);

private:
    /* Why an entry can't be inserted at key, or std::nullopt if it can.
     * The caller holds mutex_, shared or exclusively. */
    [[nodiscard]] std::optional<RegisterResult> refusal(uintptr_t key) const HIPOBJ_REQUIRES_SHARED(mutex_);
    /* refusal(), with mutex_ held shared. A registration checks it before
     * registering, so a duplicate or a full map doesn't register for
     * nothing, and insert() checks again. */
    [[nodiscard]] std::optional<RegisterResult> precheck(uintptr_t key) const HIPOBJ_REQUIRES(!mutex_);
    /* Inserts entry at key, unless another thread registered key, or
     * filled the map, after precheck(). An entry it doesn't insert is
     * released after mutex_ is. */
    [[nodiscard]] RegisterResult insert(uintptr_t key, std::shared_ptr<const BufEntry> entry)
        HIPOBJ_REQUIRES(!mutex_);

    const Verbs      *verbs_;
    const HipRuntime *runtime_;
    /* Guards entries_. Lookups take it shared, so they don't wait for each
     * other; insertions and removals take it exclusively. */
    mutable SharedMutex                                  mutex_{};
    std::map<uintptr_t, std::shared_ptr<const BufEntry>> entries_ HIPOBJ_GUARDED_BY(mutex_){};
    DmabufPolicy                                         dmabuf_;
    bool                                                 requireGpuDirect_;
};

} // namespace hipObj
