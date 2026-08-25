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

} // namespace

int BufferMap::registerBuffer(void* devPtr, size_t size, struct ibv_pd* pd) {
  if (size > MAX_MR_SIZE) {
    return -1;
  }
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  if (entries_.find(key) != entries_.end()) {
    return -1;
  }
  int access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_LOCAL_WRITE;
  struct ibv_mr* mr = ibv.reg_mr(pd, devPtr, size, access);
  if (mr) {
    entries_[key] = {mr, size, true};
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
    (void)hipHostFree(hostBuf);
    return -1;
  }
  entries_[key] = {mr, size, false};
  return 0;
}

int BufferMap::deregisterBuffer(void* devPtr) {
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return -1;
  }
  BufEntry& ent = it->second;
  void* hostBuf = (!ent.isDmabuf) ? ent.mr->addr : nullptr;
  ibv.dereg_mr(ent.mr);
  if (hostBuf) {
    (void)hipHostFree(hostBuf);
  }
  entries_.erase(it);
  return 0;
}

void BufferMap::deregisterAll() {
  for (auto& [key, ent] : entries_) {
    void* hostBuf = (!ent.isDmabuf) ? ent.mr->addr : nullptr;
    ibv.dereg_mr(ent.mr);
    if (hostBuf) {
      (void)hipHostFree(hostBuf);
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

} // namespace hipObj
