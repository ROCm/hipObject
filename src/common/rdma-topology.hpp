/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RDMA topology and NIC selection.
 */

#pragma once

#include <cstdint>

#include "ibv-core.hpp"

namespace hipObj {

enum GidPriority {
  GID_UNKNOWN = -1,
  ROCEV1_LINK_LOCAL = 0,
  ROCEV1_GLOBAL = 1,
  ROCEV2_LINK_LOCAL = 2,
  ROCEV2_GLOBAL = 3,
  ROCEV2_IPV4 = 5,
};

int GetClosestNicToGpu(int gpuIndex, const char* hca_list,
                       const char** dev_name);

int SelectBestGid(ibv_context* ctx, uint8_t port_num);

} // namespace hipObj
