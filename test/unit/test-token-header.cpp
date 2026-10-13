/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The RDMA test server's parseTokenHeader(), in
 * test/integration/rdma-test-server/token_header.h */

#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "token.h"
#include "token_header.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipobj::test::parseTokenHeader;

/* An encoded RC token with a nonzero address and length, for the suffix to
 * override or leave alone */
hipObj::RdmaToken
sampleToken()
{
    hipObj::RdmaToken t{};
    t.qpNum = 0x4321;
    t.gid.fill(0xcd);
    t.rkey       = 0x1234;
    t.remoteAddr = 0x7f0000001000;
    t.length     = 0x2000;
    t.transport  = hipObj::TransportType::RC;
    t.portNum    = 1;
    return t;
}

std::string
tokenHex()
{
    return hipObj::encodeRdmaToken(sampleToken());
}

TEST(TokenHeader, AcceptsTokenWithoutSuffix)
{
    const auto parsed = parseTokenHeader(tokenHex());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->token.qpNum, 0x4321U);
    EXPECT_EQ(parsed->token.rkey, 0x1234U);
    EXPECT_EQ(parsed->token.remoteAddr, 0x7f0000001000U);
    EXPECT_EQ(parsed->token.length, 0x2000U);
    EXPECT_EQ(parsed->length, 0U);
}

TEST(TokenHeader, SuffixOverridesAddressAndLength)
{
    const auto parsed = parseTokenHeader(tokenHex() + ":7f0000009000:40");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->token.remoteAddr, 0x7f0000009000U);
    EXPECT_EQ(parsed->token.length, 0x40U);
    EXPECT_EQ(parsed->length, 0x40U);
    /* The rest of the token is untouched */
    EXPECT_EQ(parsed->token.qpNum, 0x4321U);
    EXPECT_EQ(parsed->token.rkey, 0x1234U);
}

TEST(TokenHeader, ZeroSuffixFieldsDontOverride)
{
    const auto parsed = parseTokenHeader(tokenHex() + ":0:0");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->token.remoteAddr, 0x7f0000001000U);
    EXPECT_EQ(parsed->token.length, 0x2000U);
    EXPECT_EQ(parsed->length, 0U);
}

TEST(TokenHeader, RejectsBadToken)
{
    EXPECT_FALSE(parseTokenHeader(""));
    EXPECT_FALSE(parseTokenHeader("not-a-token"));
    EXPECT_FALSE(parseTokenHeader(tokenHex().substr(1)));
    EXPECT_FALSE(parseTokenHeader(":1000:40"));
}

TEST(TokenHeader, RejectsMalformedSuffix)
{
    /* Missing or empty fields, extra delimiters, bad characters, signs and
     * prefixes, and values too big for their types */
    for (const char *suffix : {":", ":1000", ":1000:", "::40", "::", ":1000:40:", ":1000:40:5", ":10g0:40",
                               ":1000:4z", ":+1000:40", ":1000:-40", ":0x1000:40", ": 1000:40", ":1000:40 ",
                               ":10000000000000000:40", ":1000:10000000000000000"}) {
        EXPECT_FALSE(parseTokenHeader(tokenHex() + suffix)) << "\"" << suffix << "\"";
    }
}

} // namespace
