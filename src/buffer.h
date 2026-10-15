/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Buffer registration and MR cache */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "ibv-core.h"
#include "ibv-ptr.h"
#include "replace-by-move.h"

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
struct HostBufDeleter {
    const HipRuntime *runtime = nullptr;
    void              operator()(void *hostBuf) const noexcept;
};

using HostBufPtr = std::unique_ptr<void, HostBufDeleter>;

/* False when runtime reports ptr as device memory and [ptr, ptr + size)
 * doesn't fit inside the allocation that holds ptr, or when that
 * allocation can't be found. Memory the runtime doesn't report as device
 * memory passes: registering it with the NIC checks that it's mapped. */
bool isValidDeviceRange(const HipRuntime &runtime, void *ptr, size_t size);

class BufferMap {
public:
    static constexpr size_t kMaxEntries = 256;

    /* hostBuf is declared before mr so the MR is deregistered before
     * the staging buffer it covers is freed. Move assignment destroys
     * the target first, so it releases them in that order too. */
    struct BufEntry final {
        /* Host staging buffer, or null when the NIC reaches the caller's
         * memory directly */
        HostBufPtr hostBuf = nullptr;
        IbvMrPtr   mr      = nullptr;
        size_t     size    = 0;
        /* The NIC reads and writes the caller's device memory, so a
         * transfer has to synchronize with the device */
        bool isDmabuf = false;
        /* Address to advertise to the peer. Not mr->addr:
         * ibv_reg_dmabuf_mr leaves that NULL, and on the bounce path it
         * is the host staging buffer rather than the caller's pointer. */
        uint64_t remoteAddr = 0;

        BufEntry()                            = default;
        ~BufEntry()                           = default;
        BufEntry(const BufEntry &)            = delete;
        BufEntry &operator=(const BufEntry &) = delete;
        BufEntry(BufEntry &&) noexcept        = default;
        BufEntry &operator=(BufEntry &&other) noexcept
        {
            replaceByMove(*this, std::move(other));
            return *this;
        }
    };

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

    /* Each of these returns true on success and false on failure */
    [[nodiscard]] bool registerBuffer(void *devPtr, size_t size, struct ibv_pd *pd);
    [[nodiscard]] bool registerHostBuffer(void *hostPtr, size_t size, struct ibv_pd *pd);
    [[nodiscard]] bool deregisterBuffer(void *devPtr);
    /* The registration at devPtr, or null if there isn't one. Looked up
     * once per transfer and passed down. The entry stays valid until
     * devPtr is deregistered, which can't happen during a call that holds
     * the API lock. */
    const BufEntry *find(const void *devPtr) const;
    bool            isRegistered(const void *devPtr) const;

    size_t size() const;

private:
    const Verbs                  *verbs_;
    const HipRuntime             *runtime_;
    std::map<uintptr_t, BufEntry> entries_{};
    DmabufPolicy                  dmabuf_;
    bool                          requireGpuDirect_;
};

} // namespace hipObj
