/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>

#include "ibv-core.h"
#include "vendor-ops.h"

namespace hipObj {

bool
isBnxtDevice(uint32_t vendorId)
{
    return vendorId == kVendorIdBroadcom;
}

void
configureBnxtQp(struct ibv_qp_attr &attr)
{
    attr.path_mtu  = IBV_MTU_4096;
    attr.timeout   = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
}

} // namespace hipObj
