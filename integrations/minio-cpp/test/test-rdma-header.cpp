/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <array>

#include <gtest/gtest.h>
#include <hipobj.h>

#include "hipobj-warnings.h"
#include "token.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

TEST(HipObjMinioHeader, ParseRdmaReply200)
{
    int code = 0;
    EXPECT_EQ(hipObjParseRdmaReply("200", 3, &code).opError, hipObjSuccess);
    EXPECT_EQ(code, 200);
}

TEST(HipObjMinioHeader, ParseRdmaReply501)
{
    int code = 0;
    EXPECT_EQ(hipObjParseRdmaReply("501", 3, &code).opError, hipObjSuccess);
    EXPECT_EQ(code, 501);
}

TEST(HipObjMinioHeader, ParseRdmaReplyInvalid)
{
    int code = 0;
    EXPECT_EQ(hipObjParseRdmaReply("bogus", 5, &code).opError, hipObjInvalidValue);
}

TEST(HipObjMinioHeader, TokenClientNicFromGid)
{
    hipObj::RdmaToken token;
    token.transport = hipObj::TransportType::RC;
    token.gid[10]   = 0xff;
    token.gid[11]   = 0xff;
    token.gid[12]   = 10;
    token.gid[13]   = 0;
    token.gid[14]   = 0;
    token.gid[15]   = 5;

    const std::string    encoded = hipObj::encodeRdmaToken(token);
    std::array<char, 32> nicIp{};
    EXPECT_EQ(hipObjTokenClientNic(encoded.c_str(), nicIp.data(), nicIp.size()).opError, hipObjSuccess);
    EXPECT_STREQ(nicIp.data(), "10.0.0.5");
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
