/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * NUMA wrapper implementation via dlopen.
 */

#include "rdma-numa-wrapper.h"

#include <cstddef>

#include <dlfcn.h>

namespace hipObj {

NUMAWrapper::NUMAWrapper() {
  handle_ = dlopen("libnuma.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle_) {
    handle_ = dlopen("libnuma.so.1", RTLD_NOW | RTLD_LOCAL);
  }
  if (handle_) {
    auto* sym = dlsym(handle_, "numa_num_configured_nodes");
    numa_num_configured_nodes_ = reinterpret_cast<int (*)()>(sym);
  }
}

NUMAWrapper::~NUMAWrapper() {
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
    numa_num_configured_nodes_ = nullptr;
  }
}

int NUMAWrapper::num_configured_nodes() const {
  if (numa_num_configured_nodes_) {
    return numa_num_configured_nodes_();
  }
  return 1;
}

NUMAWrapper numa;

} // namespace hipObj
