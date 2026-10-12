/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* hipObj::formatHex() in shared/hipobj-format.h */

#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "hipobj-format.h"
#include "hipobj-warnings.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::formatHex;

TEST(FormatHex, FormatsLowercaseWithoutPadding)
{
    EXPECT_EQ(formatHex(0U), "0");
    EXPECT_EQ(formatHex(0xdeadbeefU), "deadbeef");
    EXPECT_EQ(formatHex(std::numeric_limits<uint64_t>::max()), "ffffffffffffffff");
    EXPECT_EQ(formatHex(uint8_t{0xab}), "ab");
}

TEST(FormatHex, PadsToWidth)
{
    EXPECT_EQ(formatHex(0U, 8), "00000000");
    EXPECT_EQ(formatHex(1U, 6), "000001");
    EXPECT_EQ(formatHex(uint64_t{0xc0ffee}, 16), "0000000000c0ffee");
    /* Padding isn't limited to the width of the type */
    EXPECT_EQ(formatHex(uint8_t{0xf}, 4), "000f");
}

TEST(FormatHex, DoesNotTruncateToWidth)
{
    EXPECT_EQ(formatHex(0x1234567U, 6), "1234567");
    EXPECT_EQ(formatHex(0xffU, 1), "ff");
}

} // namespace
