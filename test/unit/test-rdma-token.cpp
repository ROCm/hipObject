/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstring>

#include <gtest/gtest.h>

#include "token.h"

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

TEST(RdmaReply, DecodeHttp200) {
  int status = -1;
  EXPECT_TRUE(hipObj::decodeRdmaReply("200", 3, status));
  EXPECT_EQ(status, 0);
}

TEST(RdmaReply, DecodeHttp501NotSupported) {
  int status = 0;
  EXPECT_TRUE(hipObj::decodeRdmaReply("501", 3, status));
  EXPECT_EQ(status, -2);
}

TEST(RdmaReply, ParseHttp206) {
  int code = 0;
  EXPECT_TRUE(hipObj::parseRdmaReplyHttpCode("206", 3, code));
  EXPECT_EQ(code, 206);
}

TEST(RdmaReply, ParseHttp200WithPeerToken) {
  hipObj::RdmaToken peer;
  memset(&peer, 0, sizeof(peer));
  peer.qpNum = 7;
  std::string reply = hipObj::encodeReplyWithPeerToken(200, peer);
  int code = 0;
  EXPECT_TRUE(
    hipObj::parseRdmaReplyHttpCode(reply.c_str(), reply.size(), code));
  EXPECT_EQ(code, 200);
}

TEST(RdmaToken, FormatHeaderValue) {
  const char* token = "aa";
  void* buf = reinterpret_cast<void*>(0x7f0000001000ULL);
  std::string header = hipObj::formatRdmaHeaderValue(token, buf, 4096);
  EXPECT_EQ(header, "aa:00007f0000001000:0000000000001000");
}

TEST(RdmaReply, ParsePeerTokenFromReply) {
  hipObj::RdmaToken peer;
  memset(&peer, 0, sizeof(peer));
  peer.transport = hipObj::TRANSPORT_RC;
  peer.qpNum = 99;
  std::string reply = hipObj::encodeReplyWithPeerToken(200, peer);
  hipObj::RdmaToken parsed;
  int code = 0;
  EXPECT_TRUE(
    hipObj::parsePeerTokenFromReply(reply.c_str(), reply.size(), parsed, code));
  EXPECT_EQ(code, 200);
  EXPECT_EQ(parsed.qpNum, 99u);
  EXPECT_EQ(parsed.transport, hipObj::TRANSPORT_RC);
}

TEST(RdmaReply, LegacyOkHasNoPeerToken) {
  hipObj::RdmaToken parsed;
  int code = 0;
  EXPECT_FALSE(hipObj::parsePeerTokenFromReply("ok", 2, parsed, code));
}

TEST(RdmaToken, ParseClientNicFromGid) {
  hipObj::RdmaToken token;
  memset(&token, 0, sizeof(token));
  token.transport = hipObj::TRANSPORT_RC;
  token.gid[10] = 0xff;
  token.gid[11] = 0xff;
  token.gid[12] = 192;
  token.gid[13] = 168;
  token.gid[14] = 1;
  token.gid[15] = 42;

  std::string encoded = hipObj::encodeRdmaToken(token);
  char nicIp[32];
  EXPECT_TRUE(
    hipObj::parseClientNicFromTokenHex(encoded.c_str(), nicIp, sizeof(nicIp)));
  EXPECT_STREQ(nicIp, "192.168.1.42");
}
