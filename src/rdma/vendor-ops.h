/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA vendor identification and the vendors' QP attributes */

#pragma once

#include <cstdint>

#include "ibv-core.h"

namespace hipObj {

constexpr uint32_t kVendorIdBroadcom = 0x14E4;
constexpr uint32_t kVendorIdPensando = 0x1DD8;

bool isBnxtDevice(uint32_t vendorId);
bool isIonicDevice(uint32_t vendorId);

void configureBnxtQp(struct ibv_qp_attr &attr);
void configureIonicQp(struct ibv_qp_attr &attr);

} // namespace hipObj
