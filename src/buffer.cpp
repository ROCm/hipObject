/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <cstring>

#include <hip/hip_runtime.h>

#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-wrapper.h"

namespace hipObj {

namespace {

constexpr int IBV_ACCESS_REMOTE_READ = 0x1;
constexpr int IBV_ACCESS_REMOTE_WRITE = 0x2;
constexpr int IBV_ACCESS_LOCAL_WRITE = 0x4;

int validateRegistration(bool isRegistered, size_t entryCount, size_t size) {
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

void freeOwnedHostBuffer(void* hostBuf) {
  if (!hostBuf) {
    return;
  }
  auto freeFn = hipObj::hipOps().hipHostFree;
  if (freeFn) {
    (void)freeFn(hostBuf);
    return;
  }
  (void)hipHostFree(hostBuf);
}

} // namespace

int BufferMap::registerBuffer(void* devPtr, size_t size, struct ibv_pd* pd) {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  if (validateRegistration(entries_.find(key) != entries_.end(),
                           entries_.size(), size) != 0) {
    return -1;
  }
  int access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_LOCAL_WRITE;
  struct ibv_mr* mr = ibv.reg_mr(pd, devPtr, size, access);
  if (mr) {
    entries_[key] = {mr, size, true, false, static_cast<uint64_t>(key),
                     nullptr};
    return 0;
  }
  void* hostBuf = nullptr;
  hipError_t err = hipObj::hipOps().hipHostMalloc(&hostBuf, size,
                                                  hipHostMallocDefault);
  if (err != hipSuccess || !hostBuf) {
    return -1;
  }
  mr = ibv.reg_mr_host(pd, hostBuf, size, access);
  if (!mr) {
    freeOwnedHostBuffer(hostBuf);
    return -1;
  }
  entries_[key] = {mr, size, false, true,
                   reinterpret_cast<uint64_t>(hostBuf), hostBuf};
  return 0;
}

int BufferMap::registerHostBuffer(void* hostPtr, size_t size,
                                  struct ibv_pd* pd) {
  uintptr_t key = reinterpret_cast<uintptr_t>(hostPtr);
  if (validateRegistration(entries_.find(key) != entries_.end(),
                           entries_.size(), size) != 0) {
    return -1;
  }
  int access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_LOCAL_WRITE;
  struct ibv_mr* mr = ibv.reg_mr_host(pd, hostPtr, size, access);
  if (!mr) {
    return -1;
  }
  entries_[key] = {mr, size, false, false,
                   reinterpret_cast<uint64_t>(hostPtr), nullptr};
  return 0;
}

uint64_t BufferMap::lookupRemoteAddr(void* devPtr) const {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  return it == entries_.end() ? 0 : it->second.remoteAddr;
}

int BufferMap::deregisterBuffer(void* devPtr) {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return -1;
  }
  if (it->second.refCount > 0) {
    return -1; /* pinned by a live v2 connection */
  }
  BufEntry& ent = it->second;
  ibv.dereg_mr(ent.mr);
  if (ent.ownsHostBuf && ent.hostBuf) {
    freeOwnedHostBuffer(ent.hostBuf);
  }
  entries_.erase(it);
  return 0;
}

void BufferMap::deregisterAll() {
  for (auto& [key, ent] : entries_) {
    ibv.dereg_mr(ent.mr);
    if (ent.ownsHostBuf && ent.hostBuf) {
      freeOwnedHostBuffer(ent.hostBuf);
    }
  }
  entries_.clear();
}

struct ibv_mr* BufferMap::lookupMr(void* devPtr) {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return nullptr;
  }
  return it->second.mr;
}

void* BufferMap::lookupHostBuf(void* devPtr) const {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : it->second.hostBuf;
}

size_t BufferMap::lookupSize(void* devPtr) const {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return 0;
  }
  return it->second.size;
}

bool BufferMap::isRegistered(void* devPtr) const {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  return entries_.find(key) != entries_.end();
}

bool BufferMap::requiresDeviceSync(void* devPtr) const {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  return it != entries_.end() && it->second.isDmabuf;
}

#ifdef HIPOBJECT_V2_API
bool BufferMap::acquireMrRef(void* devPtr) {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return false;
  }
  ++it->second.refCount;
  return true;
}

bool BufferMap::releaseMrRef(void* devPtr) {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.refCount == 0) {
    return false;
  }
  --it->second.refCount;
  return true;
}

size_t BufferMap::mrRefCount(void* devPtr) const {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  return it == entries_.end() ? 0 : it->second.refCount;
}

bool BufferMap::anyPinned() const {
  for (const auto& [key, ent] : entries_) {
    if (ent.refCount > 0) {
      return true;
    }
  }
  return false;
}
#endif /* HIPOBJECT_V2_API */

size_t BufferMap::size() const {
  return entries_.size();
}
} // namespace hipObj
