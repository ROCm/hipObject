/* SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include "v3-wire.h"

using hipObj::v3::RdmaTokenV3;
using hipObj::v3::RdmaReplyV3;

TEST(WireV3Advertise, RoundTrips) {
  RdmaTokenV3 adv;
  adv.listenIp = "10.0.0.5";
  adv.listenPort = 18181;
  adv.rkey = 0xdeadbeef;
  adv.remoteAddr = 0x7f0000001000ULL;
  adv.length = 4096;

  const hipObj::v3::RdmaTokenV3Hex encoded = hipObj::v3::encodeRdmaTokenV3Hex(adv);
  // 10.0.0.5 as IPv4-mapped IPv6 (RFC 4291 2.5.5.2): 10 zero bytes,
  // 0xffff, then the 4 IPv4 bytes (0a 00 00 05).
  EXPECT_STREQ(encoded.data(),
           "00000000000000000000ffff0a000005:4705:deadbeef:"
           "00007f0000001000:0000000000001000");

  RdmaTokenV3 decoded;
  ASSERT_TRUE(
    hipObj::v3::decodeRdmaTokenV3(encoded.data(), hipObj::v3::kRdmaTokenV3HexLen, decoded));
  EXPECT_EQ(decoded.listenIp, "10.0.0.5");
  EXPECT_EQ(decoded.listenPort, 18181);
  EXPECT_EQ(decoded.rkey, 0xdeadbeefu);
  EXPECT_EQ(decoded.remoteAddr, 0x7f0000001000ULL);
  EXPECT_EQ(decoded.length, 4096u);
}

TEST(WireV3Advertise, RoundTripsIPv6) {
  RdmaTokenV3 adv;
  adv.listenIp = "fd00:726e:6574:1::5";
  adv.listenPort = 18181;
  adv.rkey = 0xdeadbeef;
  adv.remoteAddr = 0x7f0000001000ULL;
  adv.length = 4096;

  const hipObj::v3::RdmaTokenV3Hex encoded = hipObj::v3::encodeRdmaTokenV3Hex(adv);
  // fd00:726e:6574:0001:0000:0000:0000:0005 (:: expanded), concatenated.
  EXPECT_STREQ(encoded.data(),
           "fd00726e657400010000000000000005:4705:deadbeef:"
           "00007f0000001000:0000000000001000");

  RdmaTokenV3 decoded;
  ASSERT_TRUE(
    hipObj::v3::decodeRdmaTokenV3(encoded.data(), hipObj::v3::kRdmaTokenV3HexLen, decoded));
  EXPECT_EQ(decoded.listenIp, "fd00:726e:6574:1::5");
  EXPECT_EQ(decoded.listenPort, 18181);
  EXPECT_EQ(decoded.rkey, 0xdeadbeefu);
  EXPECT_EQ(decoded.remoteAddr, 0x7f0000001000ULL);
  EXPECT_EQ(decoded.length, 4096u);
}

TEST(WireV3Advertise, EmptyIpMeansUnknown) {
  RdmaTokenV3 adv;
  adv.listenIp = "";
  adv.listenPort = 0;
  adv.rkey = 1;
  adv.remoteAddr = 2;
  adv.length = 3;

  const hipObj::v3::RdmaTokenV3Hex encoded = hipObj::v3::encodeRdmaTokenV3Hex(adv);
  RdmaTokenV3 decoded;
  ASSERT_TRUE(
    hipObj::v3::decodeRdmaTokenV3(encoded.data(), hipObj::v3::kRdmaTokenV3HexLen, decoded));
  EXPECT_TRUE(decoded.listenIp.empty());
  EXPECT_EQ(decoded.listenPort, 0);
}

TEST(WireV3Advertise, RejectsMissingFields) {
  std::string tooShort = "00000000000000000000ffff0a000005:4705";
  RdmaTokenV3 decoded;
  EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3(tooShort.c_str(), tooShort.size(),
                                             decoded));
}

TEST(WireV3Advertise, RejectsMalformedHex) {
  std::string bad =
    "00000000000000000000ffff0a000005:4705:notahexrkey:0:0";
  RdmaTokenV3 decoded;
  EXPECT_FALSE(
    hipObj::v3::decodeRdmaTokenV3(bad.c_str(), bad.size(), decoded));
}

TEST(WireV3Advertise, RejectsWrongLengthIp) {
  // ip field must be exactly 32 hex chars (16 bytes); this one is short.
  std::string bad = "0a000005:4705:deadbeef:0:0";
  RdmaTokenV3 decoded;
  EXPECT_FALSE(
    hipObj::v3::decodeRdmaTokenV3(bad.c_str(), bad.size(), decoded));
}

TEST(WireV3Reply, RoundTrips) {
  RdmaReplyV3 reply;
  reply.httpStatus = 200;
  reply.bytesTransferred = 65536;

  std::string encoded = hipObj::v3::encodeRdmaReplyV3(reply);
  EXPECT_EQ(encoded, "200:0000000000010000");

  RdmaReplyV3 decoded;
  ASSERT_TRUE(
    hipObj::v3::decodeRdmaReplyV3(encoded.c_str(), encoded.size(), decoded));
  EXPECT_EQ(decoded.httpStatus, 200);
  EXPECT_EQ(decoded.bytesTransferred, 65536u);
}

TEST(WireV3Reply, RejectsMissingFields) {
  std::string bad = "ok";
  RdmaReplyV3 decoded;
  EXPECT_FALSE(hipObj::v3::decodeRdmaReplyV3(bad.c_str(), bad.size(), decoded));
}

TEST(WireV3Reply, RoundTripsFailureStatus) {
  RdmaReplyV3 reply;
  reply.httpStatus = 503;
  reply.bytesTransferred = 0;

  std::string encoded = hipObj::v3::encodeRdmaReplyV3(reply);
  RdmaReplyV3 decoded;
  ASSERT_TRUE(
    hipObj::v3::decodeRdmaReplyV3(encoded.c_str(), encoded.size(), decoded));
  EXPECT_EQ(decoded.httpStatus, 503);
  EXPECT_EQ(decoded.bytesTransferred, 0u);
}
