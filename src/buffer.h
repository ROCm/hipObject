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
  int registerBuffer(void* devPtr, size_t size, struct ibv_pd* pd);
  int deregisterBuffer(void* devPtr);
  void deregisterAll();
  struct ibv_mr* lookupMr(void* devPtr);
  size_t lookupSize(void* devPtr) const;
  bool isRegistered(void* devPtr) const;

private:
  struct BufEntry {
    struct ibv_mr* mr;
    size_t size;
    bool isDmabuf;
  };

  std::map<uintptr_t, BufEntry> entries_;
};

} // namespace hipObj
