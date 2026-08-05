/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>

#include "hipobj.h"

namespace hipObj {

struct DriverState {
  bool initialized = false;
  int gpuDevice = 0;
  std::string endpoint;
  std::string region;
  std::string nicHint;
  int nicIndex = -1;
  uint32_t flags = 0;
};

DriverState& getState();

} // namespace hipObj
