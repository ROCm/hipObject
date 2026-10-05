/* SPDX-License-Identifier: MIT
 */

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "v3-wire.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

using hipObj::v3::RdmaReplyV3;
using hipObj::v3::RdmaTokenV3;

TEST(WireV3Advertise, RoundTrips)
{
    RdmaTokenV3 adv;
    adv.listenIp   = "10.0.0.5";
    adv.listenPort = 18181;
    adv.rkey       = 0xdeadbeef;
    adv.remoteAddr = 0x7f0000001000ULL;
    adv.length     = 4096;

    const hipObj::v3::RdmaTokenV3Hex encoded = hipObj::v3::encodeRdmaTokenV3Hex(adv);
    // 10.0.0.5 as IPv4-mapped IPv6 (RFC 4291 2.5.5.2): 10 zero bytes,
    // 0xffff, then the 4 IPv4 bytes (0a 00 00 05).
    EXPECT_STREQ(encoded.data(), "00000000000000000000ffff0a000005:4705:deadbeef:"
                                 "00007f0000001000:0000000000001000");

    const std::optional<RdmaTokenV3> decoded = hipObj::v3::decodeRdmaTokenV3(hipObj::cStringView(encoded));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->listenIp, "10.0.0.5");
    EXPECT_EQ(decoded->listenPort, 18181);
    EXPECT_EQ(decoded->rkey, 0xdeadbeefU);
    EXPECT_EQ(decoded->remoteAddr, 0x7f0000001000ULL);
    EXPECT_EQ(decoded->length, 4096U);
}

TEST(WireV3Advertise, RoundTripsIPv6)
{
    RdmaTokenV3 adv;
    adv.listenIp   = "fd00:726e:6574:1::5";
    adv.listenPort = 18181;
    adv.rkey       = 0xdeadbeef;
    adv.remoteAddr = 0x7f0000001000ULL;
    adv.length     = 4096;

    const hipObj::v3::RdmaTokenV3Hex encoded = hipObj::v3::encodeRdmaTokenV3Hex(adv);
    // fd00:726e:6574:0001:0000:0000:0000:0005 (:: expanded), concatenated.
    EXPECT_STREQ(encoded.data(), "fd00726e657400010000000000000005:4705:deadbeef:"
                                 "00007f0000001000:0000000000001000");

    const std::optional<RdmaTokenV3> decoded = hipObj::v3::decodeRdmaTokenV3(hipObj::cStringView(encoded));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->listenIp, "fd00:726e:6574:1::5");
    EXPECT_EQ(decoded->listenPort, 18181);
    EXPECT_EQ(decoded->rkey, 0xdeadbeefU);
    EXPECT_EQ(decoded->remoteAddr, 0x7f0000001000ULL);
    EXPECT_EQ(decoded->length, 4096U);
}

TEST(WireV3Advertise, EmptyIpMeansUnknown)
{
    RdmaTokenV3 adv;
    adv.listenIp   = "";
    adv.listenPort = 0;
    adv.rkey       = 1;
    adv.remoteAddr = 2;
    adv.length     = 3;

    const hipObj::v3::RdmaTokenV3Hex encoded = hipObj::v3::encodeRdmaTokenV3Hex(adv);
    const std::optional<RdmaTokenV3> decoded = hipObj::v3::decodeRdmaTokenV3(hipObj::cStringView(encoded));
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->listenIp.empty());
    EXPECT_EQ(decoded->listenPort, 0);
}

namespace {

/* A well-formed token to break one field of at a time */
std::string
goodToken()
{
    RdmaTokenV3 adv;
    adv.listenIp   = "10.0.0.5";
    adv.listenPort = 18181;
    adv.rkey       = 0xdeadbeef;
    adv.remoteAddr = 0x7f0000001000ULL;
    adv.length     = 4096;
    return std::string(hipObj::cStringView(hipObj::v3::encodeRdmaTokenV3Hex(adv)));
}

} // namespace

TEST(WireV3Advertise, RejectsMissingFields)
{
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3("00000000000000000000ffff0a000005:4705"));
}

TEST(WireV3Advertise, RejectsMalformedHex)
{
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3("00000000000000000000ffff0a000005:4705:notahexrkey:0:0"));
}

TEST(WireV3Advertise, RejectsWrongLengthIp)
{
    // ip field must be exactly 32 hex chars (16 bytes); this one is short.
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3("0a000005:4705:deadbeef:0:0"));
}

/* The token's length is right, but a field is one digit short and its
 * neighbour one long, so only the field widths catch it */
TEST(WireV3Advertise, RejectsWrongFieldWidths)
{
    const std::string token = goodToken();
    ASSERT_TRUE(hipObj::v3::decodeRdmaTokenV3(token));
    // ip 31 digits, port 5
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3("00000000000000000000ffff0a00000:54705:deadbeef:"
                                               "00007f0000001000:0000000000001000"));
    // rkey 7 digits, remoteAddr 17
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3("00000000000000000000ffff0a000005:4705:deadbee:"
                                               "f00007f0000001000:0000000000001000"));
    // length 15 digits, with the token one short
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3(token.substr(0, token.size() - 1)));
    // length 17 digits, with the token one long
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3(token + "0"));
}

TEST(WireV3Advertise, RejectsExtraField)
{
    // Six fields, still 80 characters: the last two share the length's digits
    EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3("00000000000000000000ffff0a000005:4705:deadbeef:"
                                               "00007f0000001000:00000000:0001000"));
}

/* Every field has to be all hex digits, with no sign, whitespace or "0x"
 * that a C conversion would accept: at the start of the ip field and inside
 * it, and at the start of each other field */
TEST(WireV3Advertise, RejectsSignsAndWhitespaceInFields)
{
    const std::string token = goodToken();
    // ip is at 0, port at 33, rkey at 38, remoteAddr at 47, length at 64
    for (const size_t at : {0U, 22U, 33U, 38U, 47U, 64U}) {
        for (const std::string_view bad : {"-f", "+f", " f", "0x"}) {
            std::string broken = token;
            broken.replace(at, bad.size(), bad);
            EXPECT_FALSE(hipObj::v3::decodeRdmaTokenV3(broken)) << "\"" << broken << "\"";
        }
    }
}

TEST(WireV3Reply, RoundTrips)
{
    RdmaReplyV3 reply;
    reply.httpStatus       = 200;
    reply.bytesTransferred = 65536;

    const std::string encoded = hipObj::v3::encodeRdmaReplyV3(reply);
    EXPECT_EQ(encoded, "200:0000000000010000");

    const std::optional<RdmaReplyV3> decoded = hipObj::v3::decodeRdmaReplyV3(encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->httpStatus, 200);
    EXPECT_EQ(decoded->bytesTransferred, 65536U);
}

TEST(WireV3Reply, RoundTripsFailureStatus)
{
    RdmaReplyV3 reply;
    reply.httpStatus       = 503;
    reply.bytesTransferred = 0;

    const std::string                encoded = hipObj::v3::encodeRdmaReplyV3(reply);
    const std::optional<RdmaReplyV3> decoded = hipObj::v3::decodeRdmaReplyV3(encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->httpStatus, 503);
    EXPECT_EQ(decoded->bytesTransferred, 0U);
}

TEST(WireV3Reply, IgnoresTrailingNulAndLineEnd)
{
    using namespace std::string_view_literals;
    for (const std::string_view reply : {
             "200:0000000000001000\0"sv,
             "200:0000000000001000\r\n"sv,
             "200:0000000000001000\n"sv,
         }) {
        const std::optional<RdmaReplyV3> decoded = hipObj::v3::decodeRdmaReplyV3(reply);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->httpStatus, 200);
        EXPECT_EQ(decoded->bytesTransferred, 0x1000U);
    }
}

TEST(WireV3Reply, RejectsMissingFields)
{
    EXPECT_FALSE(hipObj::v3::decodeRdmaReplyV3("ok"));
}

/* What a C conversion would have taken: a status that wraps an int, a
 * signed or prefixed byte count, too many or too few digits, and leading
 * whitespace or signs */
TEST(WireV3Reply, RejectsMalformedFields)
{
    for (const std::string_view bad : {
             "4294967496:0000000000001000",
             "200:-1",
             "200:-000000000000001",
             "200:0x10",
             "200:0x00000000000010",
             "200: 10",
             "200: 000000000000010",
             "200:00000000000010000",
             "200:000000000000010000",
             "200:000000000000100",
             ":junk",
             " 200:0000000000001000",
             "+200:0000000000001000",
             "+20:0000000000001000",
             "-20:0000000000001000",
             "200:0000000000001000:0",
             "200:000000000000g000",
         }) {
        EXPECT_FALSE(hipObj::v3::decodeRdmaReplyV3(bad)) << "\"" << bad << "\"";
    }
}

TEST(WireV3Reply, RejectsStatusOutOfRange)
{
    for (const std::string_view bad : {
             "099:0000000000000000",
             "600:0000000000000000",
             "999:0000000000000000",
             "20:0000000000000000",
             "2000:0000000000000000",
         }) {
        EXPECT_FALSE(hipObj::v3::decodeRdmaReplyV3(bad)) << "\"" << bad << "\"";
    }
    EXPECT_TRUE(hipObj::v3::decodeRdmaReplyV3("100:0000000000000000"));
    EXPECT_TRUE(hipObj::v3::decodeRdmaReplyV3("599:0000000000000000"));
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
