/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * hipObject loads libibverbs the first time ibvAvailable() or ibv() is
 * called, not when the program starts. This is a program of its own, so
 * that nothing calls either before the test does.
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

/* Disabled: the RC-CM listener (rc-listener.cpp) calls librdmacm directly,
 * so the library and the unit tests link librdmacm and libibverbs, and the
 * dynamic loader loads libibverbs at startup. Re-enable it once librdmacm is
 * loaded on first use too. */
TEST(IbvLazyLoad, DISABLED_LibibverbsLoadsOnFirstUse)
{
    EXPECT_FALSE(libibverbsIsLoaded()) << "libibverbs was loaded before ibvAvailable() was called";

    if (!hipObj::ibvAvailable()) {
        GTEST_SKIP() << "libibverbs couldn't be loaded";
    }
    EXPECT_TRUE(libibverbsIsLoaded());

    /* Loaded once */
    const hipObj::Verbs &verbs = hipObj::ibv();
    EXPECT_TRUE(hipObj::ibvAvailable());
    EXPECT_EQ(&hipObj::ibv(), &verbs);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
