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

/* Returns the library's driver state. Tests can install a substitute
 * (setStateForTest) to observe or reset the global without touching
 * the real singleton. */
DriverState& getState();

/* Installs a state override and returns the previously installed one
 * (nullptr when the real singleton was active). Passing nullptr
 * restores the singleton. Unit tests only; production never calls it. */
DriverState* setStateForTest(DriverState* state);

} // namespace hipObj
