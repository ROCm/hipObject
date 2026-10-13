/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * hipObject loads libibverbs the first time ibv() is called, not when the
 * program starts. This is a program of its own, so that nothing calls
 * ibv() before the test does.
 */

#include <memory>

#include <gtest/gtest.h>

#include <dlfcn.h>

#include "dl-handle.h"
#include "hipobj-warnings.h"
#include "ibv-wrapper.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

bool
libibverbsIsLoaded()
{
    /* RTLD_NOLOAD only finds a library that's already loaded */
    const hipObj::DlHandle handle(dlopen("libibverbs.so.1", RTLD_NOW | RTLD_NOLOAD));
    return handle != nullptr;
}

TEST(IbvLazyLoad, LibibverbsLoadsOnFirstUse)
{
    EXPECT_FALSE(libibverbsIsLoaded()) << "libibverbs was loaded before ibv() was called";

    const hipObj::IBVWrapper &wrapper = hipObj::ibv();
    EXPECT_EQ(&hipObj::ibv(), &wrapper);
    if (!wrapper.is_initialized()) {
        GTEST_SKIP() << "libibverbs couldn't be loaded";
    }
    EXPECT_TRUE(libibverbsIsLoaded());
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
