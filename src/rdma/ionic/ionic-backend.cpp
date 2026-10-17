/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>

#include "ibv-core.h"
#include "vendor-ops.h"

namespace hipObj {

bool
isIonicDevice(uint32_t vendorId)
{
    return vendorId == kVendorIdPensando;
}

void
configureIonicQp(ibv_qp_attr &attr)
{
    attr.path_mtu  = IBV_MTU_4096;
    attr.timeout   = kQpTimeout;
    attr.retry_cnt = kQpRetryCount;
    attr.rnr_retry = kQpRnrRetry;
}

} // namespace hipObj
