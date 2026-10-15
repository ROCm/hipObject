/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "vendor-ops.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

TEST(VendorOps, VendorIdBroadcom)
{
    EXPECT_EQ(hipObj::kVendorIdBroadcom, static_cast<uint32_t>(0x14E4));
}

TEST(VendorOps, VendorIdPensando)
{
    EXPECT_EQ(hipObj::kVendorIdPensando, static_cast<uint32_t>(0x1DD8));
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
