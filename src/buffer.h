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

/* Frees a host staging buffer allocated with hipHostMalloc() */
struct HostBufDeleter {
    void operator()(void *hostBuf) const noexcept;
};

using HostBufPtr = std::unique_ptr<void, HostBufDeleter>;

/* False when ptr is device memory and [ptr, ptr + size) doesn't fit inside
 * the HIP allocation that holds ptr, or when that allocation can't be
 * found. Memory the HIP runtime doesn't report as device memory passes:
 * registering it with the NIC checks that it's mapped. */
bool isValidDeviceRange(void *ptr, size_t size);

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
        size_t   refCount   = 0; /* pinned by live v2 connections */

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

    int  registerBuffer(void *devPtr, size_t size, struct ibv_pd *pd);
    int  registerHostBuffer(void *hostPtr, size_t size, struct ibv_pd *pd);
    int  deregisterBuffer(void *devPtr);
    void deregisterAll();
    /* The registration at devPtr, or null if there isn't one. Looked up
     * once per transfer and passed down. The entry stays valid until
     * devPtr is deregistered, which can't happen during a call that holds
     * the API lock. */
    const BufEntry *find(const void *devPtr) const;
    bool            isRegistered(const void *devPtr) const;

#ifdef HIPOBJECT_V2_API
    /* v2: the shared device may close only when no MR and no
     * connection remain. Connections pin the buffers they transfer
     * with ref entries. */
    bool   acquireMrRef(void *devPtr);
    bool   releaseMrRef(void *devPtr);
    size_t mrRefCount(void *devPtr) const;
    bool   anyPinned() const;
#endif
    size_t size() const;

private:
    std::map<uintptr_t, BufEntry> entries_;
};

} // namespace hipObj
