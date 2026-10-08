/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include "hipobj-warnings.h"

namespace hipObj {

// State override installed by tests; null means the real singleton is
// active. The library itself never swaps the pointer.
static DriverState *g_state_override = nullptr;

DriverState &
getState()
{
    // Holds only strings and scalars, so destroying it at exit is safe
    HIPOBJ_WARN_NO_EXIT_DTOR_OFF
    static DriverState state;
    HIPOBJ_WARN_NO_EXIT_DTOR_ON
    return g_state_override ? *g_state_override : state;
}

DriverState *
setStateForTest(DriverState *state)
{
    DriverState *previous = g_state_override;
    g_state_override      = state;
    return previous;
}

} // namespace hipObj
