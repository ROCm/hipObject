/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for ScopeGuard: it runs its cleanup once on every way out
 * of a scope, unless it was dismissed.
 */

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "scope-guard.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::ScopeGuard;

/* Returns early, the way a failure path does, when fail is true */
void
guardedWork(int &cleanups, bool fail)
{
    ScopeGuard guard([&cleanups] { ++cleanups; });
    if (fail) {
        return;
    }
    guard.dismiss();
}

/* Builds a result, or throws, the way building a reply can */
std::string
buildResult(bool fail)
{
    if (fail) {
        throw std::runtime_error("failure");
    }
    return "result";
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
    guardedWork(cleanups, true);
    EXPECT_EQ(cleanups, 1);
}

TEST(ScopeGuard, DismissedDoesNotRun)
{
    int cleanups = 0;
    guardedWork(cleanups, false);
    EXPECT_EQ(cleanups, 0);
}

TEST(ScopeGuard, DismissAfterPassesTheValueThrough)
{
    int cleanups = 0;
    {
        ScopeGuard guard([&cleanups] { ++cleanups; });
        EXPECT_EQ(guard.dismissAfter(buildResult(false)), "result");
    }
    EXPECT_EQ(cleanups, 0);
}

TEST(ScopeGuard, DismissAfterRunsWhenTheValueThrows)
{
    int cleanups = 0;
    try {
        ScopeGuard guard([&cleanups] { ++cleanups; });
        (void)guard.dismissAfter(buildResult(true));
    }
    catch (const std::runtime_error &) {
        EXPECT_EQ(cleanups, 1);
    }
    EXPECT_EQ(cleanups, 1);
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
