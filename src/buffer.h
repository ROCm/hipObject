/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* GPU buffer registration and MR cache */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

#include "ibv-core.h"

namespace hipObj {

class BufferMap {
public:
  static constexpr size_t kMaxEntries = 256;

  int registerBuffer(void* devPtr, size_t size, struct ibv_pd* pd);
  int deregisterBuffer(void* devPtr);
  void deregisterAll();
  struct ibv_mr* lookupMr(void* devPtr);
  size_t lookupSize(void* devPtr) const;
  bool isRegistered(void* devPtr) const;
  /* True when the entry's MR covers the device pointer itself (not a
   * host-substituted fallback allocation). v2 admission requires this:
   * advertising a device address with a host MR's rkey would fail the
   * peer's protection checks. */
  bool isDeviceBacked(void* devPtr) const;

#ifdef HIPOBJECT_V2_API
  /* v2: the shared device may close only when no MR and no
   * connection remain. Connections pin the buffers they transfer
   * with ref entries. */
  bool acquireMrRef(void* devPtr);
  bool releaseMrRef(void* devPtr);
  size_t mrRefCount(void* devPtr) const;
  bool anyPinned() const;
#endif
  size_t size() const;

private:
  struct BufEntry {
    struct ibv_mr* mr;
    size_t size;
    bool isDmabuf;
    size_t refCount = 0; /* pinned by live v2 connections */
  };

  std::map<uintptr_t, BufEntry> entries_;
};

} // namespace hipObj
