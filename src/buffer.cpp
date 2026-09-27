/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <cstdio>
#include <cstdlib>
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

/* Is this pointer device memory? Answered through the seam so unit tests can
 * drive both branches without a GPU. A runtime that cannot tell us is treated
 * as "not device memory": the strict check below must not fail a host buffer
 * it merely failed to classify. */
bool isDevicePointer(void* ptr) {
  HipOps& ops = hipOps();
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
bool requireGpuDirect() {
  static bool required = [] {
    const char* env = getenv("HIPOBJ_REQUIRE_GPU_DIRECT");
    return env && *env && env[0] != '0' && env[0] != 'n' && env[0] != 'N';
  }();
  return required;
}

} // namespace

int BufferMap::registerBuffer(void* devPtr, size_t size, struct ibv_pd* pd) {
  if (size > MAX_MR_SIZE) {
    return -1;
  }
  uintptr_t key = reinterpret_cast<uintptr_t>(devPtr);
  if (entries_.find(key) != entries_.end()) {
    return -1;
  }
  if (entries_.size() >= kMaxEntries) {
    return -1;
  }
  int access = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_LOCAL_WRITE;
  struct ibv_mr* mr = ibv.reg_mr(pd, devPtr, size, access);
  if (mr) {
    entries_[key] = {mr, size, true, static_cast<uint64_t>(key), nullptr};
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
  entries_[key] = {mr, size, false, reinterpret_cast<uint64_t>(hostBuf),
                   hostBuf};
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
  if (ent.hostBuf) {
    (void)hipHostFree(ent.hostBuf);
  }
  entries_.erase(it);
  return 0;
}

void BufferMap::deregisterAll() {
  for (auto& [key, ent] : entries_) {
    ibv.dereg_mr(ent.mr);
    if (ent.hostBuf) {
      (void)hipHostFree(ent.hostBuf);
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
