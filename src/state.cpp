/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

namespace hipObj {

// State override installed by tests; null means the real singleton is
// active. The library itself never swaps the pointer.
static DriverState* g_state_override = nullptr;

DriverState& getState() {
  static DriverState state;
  return g_state_override ? *g_state_override : state;
}

DriverState* setStateForTest(DriverState* state) {
  DriverState* previous = g_state_override;
  g_state_override = state;
  return previous;
}

} // namespace hipObj
