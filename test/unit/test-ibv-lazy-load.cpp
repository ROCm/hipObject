/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * hipObject loads libibverbs when it builds the system platform, on the
 * first systemPlatform() call, not when the program starts or when
 * hipObjInit() rejects its arguments. This is a program of its own, so
 * that nothing builds the platform before the test does.
 */

#include <memory>

#include <gtest/gtest.h>

#include <dlfcn.h>

#include "dl-handle.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "state.h"

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
    EXPECT_FALSE(libibverbsIsLoaded()) << "libibverbs was loaded before the test started";

    /* hipObjInit() checks its arguments first */
    EXPECT_EQ(hipObjInit(nullptr).opError, hipObjInvalidValue);
    EXPECT_FALSE(libibverbsIsLoaded()) << "libibverbs was loaded before the system platform was built";

    const std::shared_ptr<const hipObj::Platform> platform = hipObj::systemPlatform();
    if (!platform) {
        GTEST_SKIP() << "libibverbs couldn't be loaded";
    }
    EXPECT_TRUE(libibverbsIsLoaded());

    /* Built once */
    EXPECT_EQ(hipObj::systemPlatform(), platform);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
