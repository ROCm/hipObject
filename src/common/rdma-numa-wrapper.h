/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * NUMA wrapper via dlopen of libnuma.
 */

#pragma once

#include "dl-handle.h"

namespace hipObj {

class NUMAWrapper {
public:
    NUMAWrapper();
    ~NUMAWrapper() = default;

    /* There is one instance, the global numa. A moved-from wrapper would
     * keep its function pointer without keeping libnuma loaded. */
    NUMAWrapper(const NUMAWrapper &)            = delete;
    NUMAWrapper &operator=(const NUMAWrapper &) = delete;
    NUMAWrapper(NUMAWrapper &&)                 = delete;
    NUMAWrapper &operator=(NUMAWrapper &&)      = delete;

    int num_configured_nodes() const;

private:
    DlHandle handle_;
    int (*numa_num_configured_nodes_)() = nullptr;
};

extern NUMAWrapper numa;

} // namespace hipObj
