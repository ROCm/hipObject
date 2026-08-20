/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * NUMA wrapper via dlopen of libnuma.
 */

#pragma once

namespace hipObj {

class NUMAWrapper {
public:
  NUMAWrapper();
  ~NUMAWrapper();

  int num_configured_nodes() const;

private:
  void* handle_ = nullptr;
  int (*numa_num_configured_nodes_)() = nullptr;
};

extern NUMAWrapper numa;

} // namespace hipObj
