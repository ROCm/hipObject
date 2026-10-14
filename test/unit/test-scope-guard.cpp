/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for ScopeGuard: it runs its cleanup once on every way out
 * of a scope.
 */

#include <stdexcept>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "scope-guard.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::ScopeGuard;

/* Returns -1 early, the way a failure path does, when fail is true, and
 * 0 otherwise */
int
guardedWork(int &cleanups, bool fail)
{
    const ScopeGuard guard([&cleanups] { ++cleanups; });
    if (fail) {
        return -1;
    }
    return 0;
}

} // namespace

TEST(ScopeGuard, RunsAtScopeExit)
{
    int cleanups = 0;
    {
        const ScopeGuard guard([&cleanups] { ++cleanups; });
        EXPECT_EQ(cleanups, 0);
    }
    EXPECT_EQ(cleanups, 1);
}

TEST(ScopeGuard, RunsOnEarlyReturn)
{
    int cleanups = 0;
    EXPECT_EQ(guardedWork(cleanups, true), -1);
    EXPECT_EQ(cleanups, 1);
    EXPECT_EQ(guardedWork(cleanups, false), 0);
    EXPECT_EQ(cleanups, 2);
}

TEST(ScopeGuard, RunsWhenAnExceptionUnwinds)
{
    int cleanups = 0;
    try {
        const ScopeGuard guard([&cleanups] { ++cleanups; });
        throw std::runtime_error("failure");
    }
    catch (const std::runtime_error &) {
        EXPECT_EQ(cleanups, 1);
    }
    EXPECT_EQ(cleanups, 1);
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
