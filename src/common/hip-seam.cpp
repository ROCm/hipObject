/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hip-seam.h"

namespace hipObj {

void
hipOpsDefaults(HipOps &ops)
{
    ops.hipGetDevice            = &hipGetDevice;
    ops.hipGetDeviceCount       = &hipGetDeviceCount;
    ops.hipDeviceGetPCIBusId    = &hipDeviceGetPCIBusId;
    ops.hipHostMalloc           = &hipHostMalloc;
    ops.hipHostFree             = &hipHostFree;
    ops.hipFree                 = &hipFree;
    ops.hipDeviceSynchronize    = &hipDeviceSynchronize;
    ops.hipMemcpy               = &hipMemcpy;
    ops.hipMemcpyAsync          = &hipMemcpyAsync;
    ops.hipEventCreate          = &hipEventCreate;
    ops.hipEventRecord          = &hipEventRecord;
    ops.hipEventQuery           = &hipEventQuery;
    ops.hipEventDestroy         = &hipEventDestroy;
    ops.hipPointerGetAttributes = &hipPointerGetAttributes;
    ops.hipMemGetAddressRange   = &hipMemGetAddressRange;
}

HipOps &
hipOps()
{
    static HipOps ops;
    if (ops.hipGetDevice == nullptr) {
        hipOpsDefaults(ops);
    }
    return ops;
}

} // namespace hipObj
