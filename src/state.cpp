/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

namespace hipObj {

DriverState& getState() {
  static DriverState state;
  return state;
}

} // namespace hipObj
