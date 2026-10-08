/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * NUMA wrapper implementation via dlopen.
 */

#include "rdma-numa-wrapper.h"

#include <dlfcn.h>

#include "hipobj-warnings.h"

namespace hipObj {

NUMAWrapper::NUMAWrapper()
{
    handle_.reset(dlopen("libnuma.so", RTLD_NOW | RTLD_LOCAL));
    if (!handle_) {
        handle_.reset(dlopen("libnuma.so.1", RTLD_NOW | RTLD_LOCAL));
    }
    if (handle_) {
        auto *sym                  = dlsym(handle_.get(), "numa_num_configured_nodes");
        numa_num_configured_nodes_ = reinterpret_cast<int (*)()>(sym);
    }
}

int
NUMAWrapper::num_configured_nodes() const
{
    if (numa_num_configured_nodes_) {
        return numa_num_configured_nodes_();
    }
    return 1;
}

/* Global by design; its destructor dlclose()s libnuma at exit */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF
HIPOBJ_WARN_NO_EXIT_DTOR_OFF
NUMAWrapper numa;
HIPOBJ_WARN_NO_EXIT_DTOR_ON
HIPOBJ_WARN_NO_GLOBAL_CTOR_ON

} // namespace hipObj
