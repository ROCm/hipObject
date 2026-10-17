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

/* The RC QP attributes that every vendor uses today. The local ACK
 * timeout is 4.096 us times 2 to this power, so 14 is about 67 ms */
constexpr uint8_t kQpTimeout = 14;
/* How many times a request is retried after a timeout before the QP fails.
 * 7 is the most the field holds. */
constexpr uint8_t kQpRetryCount = 7;
/* How many times a request is retried after the receiver reports that it's
 * not ready. 7 means without limit. */
constexpr uint8_t kQpRnrRetry = 7;

bool isBnxtDevice(uint32_t vendorId);
bool isIonicDevice(uint32_t vendorId);

void configureBnxtQp(struct ibv_qp_attr &attr);
void configureIonicQp(struct ibv_qp_attr &attr);

} // namespace hipObj
