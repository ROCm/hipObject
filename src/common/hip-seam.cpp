/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hip-seam.h"

namespace hipObj {

void hipOpsDefaults(HipOps& ops) {
  ops.hipGetDevice = &hipGetDevice;
  ops.hipDeviceGetPCIBusId = &hipDeviceGetPCIBusId;
  ops.hipHostMalloc = &hipHostMalloc;
  ops.hipFree = &hipFree;
  ops.hipDeviceSynchronize = &hipDeviceSynchronize;
}

HipOps& hipOps() {
  static HipOps ops;
  if (ops.hipGetDevice == nullptr) {
    hipOpsDefaults(ops);
  }
  return ops;
}

} // namespace hipObj
