/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* HIP runtime seam.
 *
 * The library calls a small set of HIP functions on its public paths.
 * Wrapping them in a table lets unit tests substitute fake
 * implementations (no GPU required) while production builds keep the
 * direct calls. The seam is intentionally minimal: only the functions
 * the library itself invokes are listed.
 *
 * Production behavior is unchanged: the default table forwards to the
 * real HIP entry points.
 */

#pragma once

#include <cstddef>

#include <hip/hip_runtime.h>

namespace hipObj {

using HipGetDeviceFn            = hipError_t (*)(int *);
using HipGetDeviceCountFn       = hipError_t (*)(int *);
using HipDeviceGetPCIBusIdFn    = hipError_t (*)(char *, int, int);
using HipHostMallocFn           = hipError_t (*)(void **, size_t, unsigned int);
using HipHostFreeFn             = hipError_t (*)(void *);
using HipFreeFn                 = hipError_t (*)(void *);
using HipDeviceSynchronizeFn    = hipError_t (*)();
using HipMemcpyFn               = hipError_t (*)(void *, const void *, size_t, hipMemcpyKind);
using HipMemcpyAsyncFn          = hipError_t (*)(void *, const void *, size_t, hipMemcpyKind, hipStream_t);
using HipEventCreateFn          = hipError_t (*)(hipEvent_t *);
using HipEventRecordFn          = hipError_t (*)(hipEvent_t, hipStream_t);
using HipEventQueryFn           = hipError_t (*)(hipEvent_t);
using HipEventDestroyFn         = hipError_t (*)(hipEvent_t);
using HipPointerGetAttributesFn = hipError_t (*)(hipPointerAttribute_t *, const void *);
using HipMemGetAddressRangeFn   = hipError_t (*)(hipDeviceptr_t *, size_t *, hipDeviceptr_t);

struct HipOps {
    HipGetDeviceFn            hipGetDevice            = nullptr;
    HipGetDeviceCountFn       hipGetDeviceCount       = nullptr;
    HipDeviceGetPCIBusIdFn    hipDeviceGetPCIBusId    = nullptr;
    HipHostMallocFn           hipHostMalloc           = nullptr;
    HipHostFreeFn             hipHostFree             = nullptr;
    HipFreeFn                 hipFree                 = nullptr;
    HipDeviceSynchronizeFn    hipDeviceSynchronize    = nullptr;
    HipMemcpyFn               hipMemcpy               = nullptr;
    HipMemcpyAsyncFn          hipMemcpyAsync          = nullptr;
    HipEventCreateFn          hipEventCreate          = nullptr;
    HipEventRecordFn          hipEventRecord          = nullptr;
    HipEventQueryFn           hipEventQuery           = nullptr;
    HipEventDestroyFn         hipEventDestroy         = nullptr;
    HipPointerGetAttributesFn hipPointerGetAttributes = nullptr;
    HipMemGetAddressRangeFn   hipMemGetAddressRange   = nullptr;
};

/* Populates a table with the real HIP entry points. Called once for the
 * global table; tests that swap pointers capture and restore the
 * original table around each case. */
void hipOpsDefaults(HipOps &ops);

/* Global table used by the library. */
HipOps &hipOps();

} // namespace hipObj
