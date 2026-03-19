/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstring>

#include <gtest/gtest.h>

#include "token.hpp"

TEST(RdmaToken, EncodeProducesHexString) {
  hipObj::RdmaToken token;
  memset(&token, 0, sizeof(token));
  token.transport = hipObj::TRANSPORT_RC;
  token.qpNum = 42;
  token.rkey = 0xDEADBEEF;
  token.remoteAddr = 0x7F0000001000ULL;
  token.length = 4096;
  token.portNum = 1;
  token.lid = 0;

  std::string encoded = hipObj::encodeRdmaToken(token);

  EXPECT_FALSE(encoded.empty());
  EXPECT_EQ(encoded.size() % 2, 0u);
  EXPECT_EQ(encoded[0], '0');
  EXPECT_EQ(encoded[1], '1');
}

TEST(RdmaToken, EncodeRcTransportByte) {
  hipObj::RdmaToken token;
  memset(&token, 0, sizeof(token));
  token.transport = hipObj::TRANSPORT_RC;

  std::string enc = hipObj::encodeRdmaToken(token);
  EXPECT_GE(enc.size(), 2u);
  EXPECT_EQ(enc.substr(0, 2), "01");
}

TEST(RdmaToken, EncodeDcTransportByte) {
  hipObj::RdmaToken token;
  memset(&token, 0, sizeof(token));
  token.transport = hipObj::TRANSPORT_DC;

  std::string enc = hipObj::encodeRdmaToken(token);
  EXPECT_GE(enc.size(), 2u);
  EXPECT_EQ(enc.substr(0, 2), "00");
}

TEST(RdmaReply, DecodeOk) {
  int status = -1;
  EXPECT_TRUE(hipObj::decodeRdmaReply("ok", 2, status));
  EXPECT_EQ(status, 0);
}

TEST(RdmaReply, DecodeErr) {
  int status = 0;
  EXPECT_TRUE(hipObj::decodeRdmaReply("err", 3, status));
  EXPECT_EQ(status, -1);
}

TEST(RdmaReply, DecodeNullReturnsFalse) {
  int status = 0;
  EXPECT_FALSE(hipObj::decodeRdmaReply(nullptr, 0, status));
}
