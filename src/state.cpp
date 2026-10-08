/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include <mutex>

#include "hipobj-warnings.h"

namespace hipObj {

// State override installed by tests; null means the real singleton is
// active. The library itself never swaps the pointer.
static DriverState *g_state_override = nullptr;

// True while the calling thread holds the API lock. Each thread has its
// own copy, so it needs no synchronization.
static thread_local bool t_holdsApiLock = false;

static std::mutex &
apiMutex()
{
    static std::mutex lock;
    return lock;
}

ApiGuard::ApiGuard()
{
    if (t_holdsApiLock) {
        return;
    }
    lock_          = std::unique_lock<std::mutex>(apiMutex());
    t_holdsApiLock = true;
}

// lock_ is destroyed, and the mutex unlocked, after this body runs
ApiGuard::~ApiGuard()
{
    if (lock_.owns_lock()) {
        t_holdsApiLock = false;
    }
}

bool
ApiGuard::owns() const noexcept
{
    return lock_.owns_lock();
}

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
