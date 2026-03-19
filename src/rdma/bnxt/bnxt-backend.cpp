/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "vendor-ops.hpp"
#include "ibv-core.hpp"
#include "ibv-wrapper.hpp"

#include <cstring>

namespace hipObj {

bool isBnxtDevice(uint32_t vendorId) {
  return vendorId == VENDOR_ID_BROADCOM;
}

int configureBnxtQp(struct ibv_qp_attr* attr) {
  if (!attr) {
    return -1;
  }
  attr->path_mtu = IBV_MTU_4096;
  attr->timeout = 14;
  attr->retry_cnt = 7;
  attr->rnr_retry = 7;
  return 0;
}

} // namespace hipObj
