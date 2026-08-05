/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstring>

#include <gtest/gtest.h>
#include <hipobj.h>

#include "token.hpp"

TEST(HipObjMinioHeader, FormatRdmaHeaderValue) {
  hipObj::RdmaToken token;
  std::memset(&token, 0, sizeof(token));
  token.transport = hipObj::TRANSPORT_RC;
  token.qpNum = 42;
  token.remoteAddr = 0x7f0000001000ULL;
  token.length = 4096;
  std::string encoded = hipObj::encodeRdmaToken(token);
  void* buf = reinterpret_cast<void*>(0x7f0000001000ULL);
  std::string header = hipObj::formatRdmaHeaderValue(encoded.c_str(), buf,
                                                     4096);
  EXPECT_EQ(header, encoded + ":00007f0000001000:0000000000001000");
}

TEST(HipObjMinioHeader, ParseRdmaReply200) {
  int code = 0;
  EXPECT_EQ(hipObjParseRdmaReply("200", 3, &code).opError, hipObjSuccess);
  EXPECT_EQ(code, 200);
}

TEST(HipObjMinioHeader, ParseRdmaReply501) {
  int code = 0;
  EXPECT_EQ(hipObjParseRdmaReply("501", 3, &code).opError, hipObjSuccess);
  EXPECT_EQ(code, 501);
}

TEST(HipObjMinioHeader, ParseRdmaReplyInvalid) {
  int code = 0;
  EXPECT_EQ(hipObjParseRdmaReply("bogus", 5, &code).opError,
            hipObjInvalidValue);
}

TEST(HipObjMinioHeader, TokenClientNicFromGid) {
  hipObj::RdmaToken token;
  std::memset(&token, 0, sizeof(token));
  token.transport = hipObj::TRANSPORT_RC;
  token.gid[10] = 0xff;
  token.gid[11] = 0xff;
  token.gid[12] = 10;
  token.gid[13] = 0;
  token.gid[14] = 0;
  token.gid[15] = 5;

  std::string encoded = hipObj::encodeRdmaToken(token);
  char nicIp[32];
  EXPECT_EQ(hipObjTokenClientNic(encoded.c_str(), nicIp, sizeof(nicIp)).opError,
            hipObjSuccess);
  EXPECT_STREQ(nicIp, "10.0.0.5");
}
