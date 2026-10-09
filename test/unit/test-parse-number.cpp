/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* hipObj::parseNumber() in shared/hipobj-parse.h */

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

#include "hipobj-parse.h"
#include "hipobj-warnings.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::parseNumber;

TEST(ParseNumber, ParsesDecimal)
{
    EXPECT_EQ(parseNumber<uint32_t>("0"), 0U);
    EXPECT_EQ(parseNumber<uint32_t>("4096"), 4096U);
    EXPECT_EQ(parseNumber<uint64_t>("18446744073709551615"), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(parseNumber<int64_t>("-42"), -42);
    /* Leading zeros don't count against the type's width */
    EXPECT_EQ(parseNumber<uint8_t>("0000000255"), 255U);
}

TEST(ParseNumber, ParsesHexInEitherCase)
{
    EXPECT_EQ(parseNumber<uint32_t>("deadBEEF", 16), 0xdeadbeefU);
    EXPECT_EQ(parseNumber<uint8_t>("ff", 16), 0xffU);
}

TEST(ParseNumber, RejectsEmptyText)
{
    EXPECT_FALSE(parseNumber<uint32_t>(""));
    /* A default-constructed string_view has a null data() */
    EXPECT_FALSE(parseNumber<uint32_t>(std::string_view{}));
}

TEST(ParseNumber, RejectsSignsPrefixesAndWhitespace)
{
    for (const char *text : {"+1", "-1", " 1", "1 ", "\t1", "1\n", "0x10", "1,000", "1.0", "1e3"}) {
        EXPECT_FALSE(parseNumber<uint32_t>(text)) << "\"" << text << "\"";
    }
    EXPECT_FALSE(parseNumber<int32_t>("+1"));
    EXPECT_FALSE(parseNumber<int32_t>("- 1"));
    EXPECT_FALSE(parseNumber<uint32_t>("0x10", 16));
    EXPECT_FALSE(parseNumber<uint32_t>("g", 16));
}

TEST(ParseNumber, RejectsValuesThatDontFit)
{
    EXPECT_FALSE(parseNumber<uint8_t>("256"));
    EXPECT_FALSE(parseNumber<uint8_t>("100", 16));
    EXPECT_FALSE(parseNumber<uint64_t>("18446744073709551616"));
    EXPECT_FALSE(parseNumber<int32_t>("-2147483649"));
    EXPECT_EQ(parseNumber<int32_t>("-2147483648"), std::numeric_limits<int32_t>::min());
}

TEST(ParseNumber, RejectsBasesFromCharsDoesntAllow)
{
    for (int base : {-16, 0, 1, 37}) {
        EXPECT_FALSE(parseNumber<uint32_t>("1", base)) << "base " << base;
    }
    EXPECT_EQ(parseNumber<uint32_t>("11", 2), 3U);
    EXPECT_EQ(parseNumber<uint32_t>("z", 36), 35U);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
