/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "token.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

TEST(RdmaToken, EncodeProducesHexString)
{
    hipObj::RdmaToken token;
    token.transport  = hipObj::TransportType::RC;
    token.qpNum      = 42;
    token.rkey       = 0xDEADBEEF;
    token.remoteAddr = 0x7F0000001000ULL;
    token.length     = 4096;
    token.portNum    = 1;
    token.lid        = 0;

    std::string encoded = hipObj::encodeRdmaToken(token);

    EXPECT_FALSE(encoded.empty());
    EXPECT_EQ(encoded.size(), 88U);
    EXPECT_EQ(encoded.size() % 2, 0U);
    EXPECT_EQ(encoded[0], '0');
    EXPECT_EQ(encoded[1], '1');
}

TEST(RdmaToken, EncodeHexIsNulTerminatedAndMatchesString)
{
    hipObj::RdmaToken token;
    token.transport  = hipObj::TransportType::RC;
    token.qpNum      = 0x01020304;
    token.gid[0]     = 0xfe;
    token.gid[15]    = 0x80;
    token.rkey       = 0xA5A5A5A5;
    token.remoteAddr = 0xFFFFFFFFFFFFFFFFULL;
    token.length     = 1;
    token.portNum    = 0xff;
    token.lid        = 0xabcd;

    const hipObj::RdmaTokenHex hex = hipObj::encodeRdmaTokenHex(token);
    EXPECT_EQ(hex[hipObj::kRdmaTokenHexLen], '\0');
    EXPECT_EQ(std::string(hex.data()), hipObj::encodeRdmaToken(token));
    for (size_t i = 0; i < hipObj::kRdmaTokenHexLen; ++i) {
        EXPECT_TRUE((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')) << i;
    }

    const auto decoded = hipObj::decodeRdmaTokenHex(hex.data());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->qpNum, token.qpNum);
    EXPECT_EQ(decoded->gid, token.gid);
    EXPECT_EQ(decoded->rkey, token.rkey);
    EXPECT_EQ(decoded->remoteAddr, token.remoteAddr);
    EXPECT_EQ(decoded->length, token.length);
    EXPECT_EQ(decoded->portNum, token.portNum);
    EXPECT_EQ(decoded->lid, token.lid);
}

TEST(RdmaToken, EncodeReplyWithPeerToken)
{
    hipObj::RdmaToken peer;
    peer.transport = hipObj::TransportType::RC;
    peer.qpNum     = 7;

    EXPECT_EQ(hipObj::encodeReplyWithPeerToken(200, peer), "200:" + hipObj::encodeRdmaToken(peer));
}

TEST(RdmaToken, EncodeRcTransportByte)
{
    hipObj::RdmaToken token;
    token.transport = hipObj::TransportType::RC;

    std::string enc = hipObj::encodeRdmaToken(token);
    EXPECT_GE(enc.size(), 2U);
    EXPECT_EQ(enc.substr(0, 2), "01");
}

TEST(RdmaToken, EncodeDcTransportByte)
{
    hipObj::RdmaToken token;
    token.transport = hipObj::TransportType::DC;

    std::string enc = hipObj::encodeRdmaToken(token);
    EXPECT_GE(enc.size(), 2U);
    EXPECT_EQ(enc.substr(0, 2), "00");
}

TEST(RdmaReply, DecodeOk)
{
    EXPECT_EQ(hipObj::decodeRdmaReply("ok"), hipObj::RdmaReplyStatus::Success);
}

TEST(RdmaReply, DecodeErr)
{
    EXPECT_EQ(hipObj::decodeRdmaReply("err"), hipObj::RdmaReplyStatus::Failed);
}

TEST(RdmaReply, DecodeEmptyFails)
{
    EXPECT_FALSE(hipObj::decodeRdmaReply({}));
}

TEST(RdmaReply, DecodeHttpStatus)
{
    EXPECT_EQ(hipObj::decodeRdmaReply("200"), hipObj::RdmaReplyStatus::Success);
    EXPECT_EQ(hipObj::decodeRdmaReply("204"), hipObj::RdmaReplyStatus::Success);
    EXPECT_EQ(hipObj::decodeRdmaReply("206"), hipObj::RdmaReplyStatus::Success);
    EXPECT_EQ(hipObj::decodeRdmaReply("404"), hipObj::RdmaReplyStatus::Failed);
}

TEST(RdmaReply, DecodeHttp501NotSupported)
{
    EXPECT_EQ(hipObj::decodeRdmaReply("501"), hipObj::RdmaReplyStatus::NotSupported);
}

TEST(RdmaReply, ParseHttp206)
{
    EXPECT_EQ(hipObj::parseRdmaReplyHttpCode("206"), 206);
}

TEST(RdmaReply, ParseHttp200WithPeerToken)
{
    hipObj::RdmaToken peer;
    peer.qpNum        = 7;
    std::string reply = hipObj::encodeReplyWithPeerToken(200, peer);
    EXPECT_EQ(hipObj::parseRdmaReplyHttpCode(reply), 200);
}

TEST(RdmaToken, FormatHeaderValue)
{
    const char *token  = "0011";
    void       *buf    = reinterpret_cast<void *>(0x7f0000001000ULL);
    std::string header = hipObj::formatRdmaHeaderValue(token, buf, 4096);
    EXPECT_EQ(header, token);
}

TEST(RdmaToken, DecodeRejectsColonSuffixedHeaderValue)
{
    hipObj::RdmaToken token{};
    std::string       encoded = hipObj::encodeRdmaToken(token);

    EXPECT_FALSE(hipObj::decodeRdmaTokenHex(encoded + ":1:2"));
    EXPECT_FALSE(hipObj::decodeRdmaTokenHex(encoded + ":00007f0000001000:0000000000001000"));
    EXPECT_TRUE(hipObj::decodeRdmaTokenHex(encoded));
}

TEST(RdmaReply, ParsePeerTokenFromReply)
{
    hipObj::RdmaToken peer;
    peer.transport     = hipObj::TransportType::RC;
    peer.qpNum         = 99;
    std::string reply  = hipObj::encodeReplyWithPeerToken(200, peer);
    const auto  parsed = hipObj::parsePeerTokenFromReply(reply);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->qpNum, 99U);
    EXPECT_EQ(parsed->transport, hipObj::TransportType::RC);
}

TEST(RdmaReply, LegacyOkHasNoPeerToken)
{
    EXPECT_FALSE(hipObj::parsePeerTokenFromReply("ok"));
}

TEST(RdmaReply, ParsePeerTokenRejectsMalformedReply)
{
    hipObj::RdmaToken peer;
    peer.transport            = hipObj::TransportType::RC;
    const std::string token   = hipObj::encodeRdmaToken(peer);
    const std::string cases[] = {
        "200",
        "+200:" + token,
        "0200:" + token,
        "20:" + token,
        "600:" + token,
        "206:" + token,
        "404:" + token,
        "200:" + token.substr(1),
        "200:" + token + "0",
        "200:03" + token.substr(2),
    };
    for (const std::string &reply : cases) {
        EXPECT_FALSE(hipObj::parsePeerTokenFromReply(reply)) << "reply \"" << reply << "\"";
    }
}

TEST(RdmaToken, DecodeRejectsUnknownTransport)
{
    hipObj::RdmaToken token;
    token.transport     = hipObj::TransportType::RC;
    std::string encoded = hipObj::encodeRdmaToken(token);
    /* The transport is the token's first byte */
    encoded.replace(0, 2, "02");

    EXPECT_FALSE(hipObj::decodeRdmaTokenHex(encoded));
}

TEST(RdmaToken, DecodeAcceptsUppercaseHex)
{
    hipObj::RdmaToken token;
    token.transport = hipObj::TransportType::RC;
    /* A made-up address with every hex letter in it. gitleaks's
     * generic-api-key rule mistakes a random-looking value assigned to a
     * "token" for a credential. */
    token.remoteAddr    = 0xabcdef0123456789ULL; // gitleaks:allow
    std::string encoded = hipObj::encodeRdmaToken(token);
    for (char &c : encoded) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    const auto parsed = hipObj::decodeRdmaTokenHex(encoded);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->remoteAddr, 0xabcdef0123456789ULL);
}

TEST(RdmaToken, DecodeRejectsBytesThatArentTwoHexDigits)
{
    hipObj::RdmaToken token;
    token.transport              = hipObj::TransportType::RC;
    const std::string encoded    = hipObj::encodeRdmaToken(token);
    const char *const badBytes[] = {"+1", "-1", " 1", "1 ", "0x", "g0", "0g"};
    for (const char *bad : badBytes) {
        /* Replace a byte in the middle of the token, in the GID */
        std::string mangled = encoded;
        mangled.replace(20, 2, bad);

        EXPECT_FALSE(hipObj::decodeRdmaTokenHex(mangled)) << "byte \"" << bad << "\"";
    }
}

TEST(RdmaReply, ParseRejectsStatusThatIsntThreeDigits)
{
    for (const char *reply : {"+20", "-20", " 20", "20 ", "2 0", "0x1", "-99"}) {
        EXPECT_FALSE(hipObj::parseRdmaReplyHttpCode(reply)) << "reply \"" << reply << "\"";
    }
}

TEST(RdmaToken, ParseClientNicFromGid)
{
    hipObj::RdmaToken token;
    token.transport = hipObj::TransportType::RC;
    token.gid[10]   = 0xff;
    token.gid[11]   = 0xff;
    token.gid[12]   = 192;
    token.gid[13]   = 168;
    token.gid[14]   = 1;
    token.gid[15]   = 42;

    std::string encoded = hipObj::encodeRdmaToken(token);
    char        nicIp[32];
    EXPECT_TRUE(hipObj::parseClientNicFromTokenHex(encoded.c_str(), nicIp, sizeof(nicIp)));
    EXPECT_STREQ(nicIp, "192.168.1.42");
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
