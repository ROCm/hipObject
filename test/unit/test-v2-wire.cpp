/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Unit tests for the hipobj-rc-v2 wire helpers (src/rdma/v2-wire.*).
 * These are pure parsing/formatting tests and need no RDMA hardware. */

#include <clocale>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "hipobj.h"
#include "v2-wire.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::v2::buildTarget;
using hipObj::v2::formatCookie;
using hipObj::v2::formatPsn;
using hipObj::v2::isValidSessionHex;
using hipObj::v2::parseFinalReply;
using hipObj::v2::parsePrepareReply;
using hipObj::v2::parsePsn;
using hipObj::v2::splitHeaderLine;
using hipObj::v2::validateChecksumText;

const char *kGoodSession = "0123456789abcdef0123456789abcdef";
const char *kGoodToken   = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
                           "2122232425262728292a2b";

std::string
prepareOkHeaders(const char *session, const char *psn)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += std::string("X-Amz-Rdma-Reply: 200:") + kGoodToken + "\r\n";
    h += std::string("X-Amz-Rdma-Session: ") + session + "\r\n";
    h += std::string("X-Amz-Rdma-Psn: ") + psn + "\r\n";
    return h;
}

TEST(V2Wire, SplitHeaderLine)
{
    auto header = splitHeaderLine("Name: value");
    ASSERT_TRUE(header);
    EXPECT_EQ(header->name, "Name");
    EXPECT_EQ(header->value, "value");
    header = splitHeaderLine("  Padded  :  v  ");
    ASSERT_TRUE(header);
    EXPECT_EQ(header->name, "Padded");
    EXPECT_EQ(header->value, "v");
    EXPECT_FALSE(splitHeaderLine("NoColon"));
    EXPECT_FALSE(splitHeaderLine(": novalue"));
}

TEST(V2Wire, PrepareOk)
{
    const auto r = parsePrepareReply(200, prepareOkHeaders(kGoodSession, "01ab01"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->httpStatus, 200);
    EXPECT_TRUE(r->protocolEcho);
    EXPECT_FALSE(r->unsupportedMarker);
    EXPECT_EQ(r->serverToken, std::string(kGoodToken));
    EXPECT_EQ(r->session, std::string(kGoodSession));
    EXPECT_EQ(r->serverPsn, 0x01ab01U);
}

TEST(V2Wire, PrepareOkCaseInsensitiveHeaderNames)
{
    std::string h = "x-aMz-rDmA-pRoToCoL: hipobj-rc-v2\r\n";
    h += std::string("x-amz-rdma-reply: 200:") + kGoodToken + "\r\n";
    h += std::string("X-AMZ-RDMA-SESSION: ") + kGoodSession + "\r\n";
    h += "x-amz-rdma-psn: ffffff\r\n";
    const auto r = parsePrepareReply(200, h);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->protocolEcho);
    EXPECT_EQ(r->serverPsn, 0xffffffU);
}

TEST(V2Wire, PrepareUnsupported)
{
    std::string h = "X-Amz-Rdma-Protocol-Status: unsupported\r\n";
    const auto  r = parsePrepareReply(501, h);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->httpStatus, 501);
    EXPECT_FALSE(r->protocolEcho);
    EXPECT_TRUE(r->unsupportedMarker);
}

TEST(V2Wire, PrepareBare501Rejected)
{
    /* An unmarked 501 (proxy error etc.) must not look like support
     * negotiation; the marker is what makes 501 protocol-specific. */
    const auto r = parsePrepareReply(501, "");
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->unsupportedMarker);
}

TEST(V2Wire, PrepareMissingEchoIsError)
{
    std::string h = std::string("X-Amz-Rdma-Reply: 200:") + kGoodToken + "\r\n" +
                    std::string("X-Amz-Rdma-Session: ") + kGoodSession + "\r\nX-Amz-Rdma-Psn: 000001\r\n";
    EXPECT_FALSE(parsePrepareReply(200, h));
}

TEST(V2Wire, PrepareZeroPsnIsError)
{
    EXPECT_FALSE(parsePrepareReply(200, prepareOkHeaders(kGoodSession, "000000")));
}

TEST(V2Wire, PrepareBadSessionRejected)
{
    EXPECT_FALSE(parsePrepareReply(200, prepareOkHeaders("0123", "000001")));
    EXPECT_FALSE(parsePrepareReply(200, prepareOkHeaders("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "000001")));
}

TEST(V2Wire, PrepareBadTokenRejected)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += "X-Amz-Rdma-Reply: 200:tooshort\r\n";
    h += std::string("X-Amz-Rdma-Session: ") + kGoodSession + "\r\n";
    h += "X-Amz-Rdma-Psn: 000001\r\n";
    EXPECT_FALSE(parsePrepareReply(200, h));
}

TEST(V2Wire, FinalGetOk)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += "X-Amz-Rdma-Cookie: 00c0ffee\r\n";
    h += "X-Amz-Rdma-Bytes-Transferred: 65536\r\n";
    h += "X-Amz-Rdma-Etag: \"abc123\"\r\n";
    const auto r = parseFinalReply(200, h);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->httpStatus, 200);
    EXPECT_TRUE(r->protocolEcho);
    EXPECT_EQ(r->cookieEcho, 0x00c0ffeeU);
    EXPECT_TRUE(r->cookiePresent);
    EXPECT_EQ(r->bytes, 65536U);
    EXPECT_EQ(r->etag, "\"abc123\"");
}

TEST(V2Wire, FinalOkCaseInsensitiveHeaderNames)
{
    std::string h = "x-amz-rdma-PROTOCOL: hipobj-rc-v2\r\n";
    h += "x-AMZ-rdma-cookie: 00c0ffee\r\n";
    h += "X-AMZ-RDMA-BYTES-TRANSFERRED: 4096\r\n";
    h += "x-amz-rdma-etag: \"abc123\"\r\n";
    h += "X-amz-Rdma-Version-ID: v7\r\n";
    h += "x-amz-rdma-checksum: CRC64NVME AAAAAAAAAAA=\r\n";
    const auto r = parseFinalReply(200, h);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->protocolEcho);
    EXPECT_EQ(r->cookieEcho, 0x00c0ffeeU);
    EXPECT_EQ(r->bytes, 4096U);
    EXPECT_EQ(r->etag, "\"abc123\"");
    EXPECT_EQ(r->versionId, "v7");
    EXPECT_EQ(r->checksumB64, "AAAAAAAAAAA=");
}

TEST(V2Wire, FinalPutOkWithChecksum)
{
    /* CRC64NVME of the empty string, canonical base64: 8 bytes -> 12 chars
     * ending with '='. Use a known-vector style constant; the parser only
     * validates canonical form. */
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += "X-Amz-Rdma-Cookie: deadbeef\r\n";
    h += "X-Amz-Rdma-Bytes-Transferred: 1024\r\n";
    h += "X-Amz-Rdma-Checksum: CRC64NVME AAAAAAAAAAA=\r\n";
    const auto r = parseFinalReply(204, h);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->checksumB64, "AAAAAAAAAAA=");
}

TEST(V2Wire, FinalChecksumNonCanonicalRejected)
{
    /* Non-canonical: last data char has nonzero pad bits. */
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += "X-Amz-Rdma-Cookie: deadbeef\r\n";
    h += "X-Amz-Rdma-Bytes-Transferred: 1024\r\n";
    h += "X-Amz-Rdma-Checksum: CRC64NVME AAAAAAAAAAB=\r\n";
    EXPECT_FALSE(parseFinalReply(204, h));
}

TEST(V2Wire, FinalChecksumBadLengthRejected)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += "X-Amz-Rdma-Cookie: deadbeef\r\n";
    h += "X-Amz-Rdma-Checksum: CRC64NVME AAAA=\r\n";
    EXPECT_FALSE(parseFinalReply(204, h));
}

TEST(V2Wire, FinalMissingCookieRejected)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += "X-Amz-Rdma-Bytes-Transferred: 1\r\n";
    EXPECT_FALSE(parseFinalReply(200, h));
}

TEST(V2Wire, FinalErrHasNoMandatoryCookie)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    const auto  r = parseFinalReply(500, h);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->protocolEcho);
    EXPECT_FALSE(r->cookiePresent);
}

TEST(V2Wire, ChecksumTextValidation)
{
    EXPECT_EQ(validateChecksumText("CRC64NVME AAAAAAAAAAA="), "AAAAAAAAAAA=");
    EXPECT_FALSE(validateChecksumText("CRC64NV AAAAAAAAAAA="));
    EXPECT_FALSE(validateChecksumText("CRC64NVME AAAAAAAAAAA"));
    EXPECT_FALSE(validateChecksumText("CRC64NVME AAAAAAAAAA=="));
    EXPECT_FALSE(validateChecksumText("CRC64NVME =AAAAAAAAAA="));
}

TEST(V2Wire, SessionHex)
{
    EXPECT_TRUE(isValidSessionHex(kGoodSession));
    EXPECT_TRUE(isValidSessionHex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));
    EXPECT_FALSE(isValidSessionHex("short"));
    EXPECT_FALSE(isValidSessionHex("g123456789abcdef0123456789abcdef"));
}

TEST(V2Wire, Psn)
{
    EXPECT_EQ(parsePsn("000001"), 1U);
    EXPECT_EQ(parsePsn("ffffff"), 0xffffffU);
    EXPECT_EQ(parsePsn("ABCDEF"), 0xabcdefU);
    EXPECT_FALSE(parsePsn("000000"));
    EXPECT_FALSE(parsePsn("1000000"));
    EXPECT_FALSE(parsePsn("zzzzzz"));
    for (const char *psn : {"+00001", "-00001", "0x0001", "00 001", " 00001", "00001 "}) {
        EXPECT_FALSE(parsePsn(psn)) << "psn \"" << psn << "\"";
    }
}

TEST(V2Wire, FormatHelpers)
{
    EXPECT_EQ(formatCookie(0xdeadbeefU), "deadbeef");
    EXPECT_EQ(formatPsn(1), "000001");
    EXPECT_EQ(formatPsn(0xffffffU), "ffffff");
    /* Out-of-range PSNs keep the low 24 bits, not the leading 6 digits */
    EXPECT_EQ(formatPsn(0x1234567U), "234567");
}

TEST(V2Wire, BuildTarget)
{
    EXPECT_EQ(buildTarget("b", "k", ""), "/b/k");
    EXPECT_EQ(buildTarget("bucket", "key name", ""), "/bucket/key%20name");
    EXPECT_EQ(buildTarget("b", "k", "partNumber=1"), "/b/k?partNumber=1");
    EXPECT_EQ(buildTarget("b", "a/b", ""), "/b/a/b");
}

// ---- Malformed replies -------------------------------------------
//
// Everything in a reply comes from the network, so a malformed field must
// make the parse fail rather than be skipped or guessed at.

std::string
finalOkHeaders()
{
    return "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\nX-Amz-Rdma-Cookie: 00c0ffee\r\n";
}

std::string
withoutFinalCrlf(std::string headers)
{
    headers.resize(headers.size() - 2);
    return headers;
}

TEST(V2Wire, FinalIgnoresNonAsciiHeaderNames)
{
    /* Header names are ASCII, so a non-ASCII letter that looks like or folds
     * to 'i' isn't the version header: 0xDD is Turkish capital dotted I in
     * ISO-8859-9, and 0xC4 0xB0 is the same letter in UTF-8 */
    for (const char *name : {"X-Amz-Rdma-Vers\xDDon-Id", "X-Amz-Rdma-Vers\xC4\xB0on-Id"}) {
        const auto r = parseFinalReply(200, finalOkHeaders() + name + ": v7\r\n");
        ASSERT_TRUE(r) << name;
        EXPECT_EQ(r->versionId, "") << name;
    }
}

TEST(V2Wire, HeaderNamesIgnoreLocale)
{
    /* The application, not the library, sets the locale. In Turkish ones,
     * std::tolower('I') isn't 'i', and in ISO-8859-9, std::tolower(0xDD) is.
     * Copy the current name first: the next setlocale() call can overwrite
     * the string it points to. */
    const std::string saved = std::setlocale(LC_ALL, nullptr);
    const char       *found = nullptr;
    for (const char *locale : {"tr_TR.ISO-8859-9", "tr_TR.UTF-8", "tr_TR"}) {
        if (std::setlocale(LC_ALL, locale) != nullptr) {
            found = locale;
            break;
        }
    }
    if (found == nullptr) {
        GTEST_SKIP() << "needs a Turkish locale, such as tr_TR.UTF-8";
    }

    const auto upper    = parseFinalReply(200, finalOkHeaders() + "X-AMZ-RDMA-VERSION-ID: v7\r\n");
    const auto nonAscii = parseFinalReply(200, finalOkHeaders() + "X-Amz-Rdma-Vers\xDDon-Id: v7\r\n");
    std::setlocale(LC_ALL, saved.c_str());

    ASSERT_TRUE(upper) << found;
    EXPECT_EQ(upper->versionId, "v7") << found;
    ASSERT_TRUE(nonAscii) << found;
    EXPECT_EQ(nonAscii->versionId, "") << found;
}

TEST(V2Wire, SplitHeaderLineRejectsEmptyName)
{
    EXPECT_FALSE(splitHeaderLine("  : value"));
    EXPECT_FALSE(splitHeaderLine("\t:value"));
}

TEST(V2Wire, PrepareToleratesBlankLinesAndMissingFinalCrlf)
{
    std::string h = "\r\n" + prepareOkHeaders(kGoodSession, "000001") + "\r\n";
    EXPECT_TRUE(parsePrepareReply(200, h));
    const auto r = parsePrepareReply(200, withoutFinalCrlf(prepareOkHeaders(kGoodSession, "000002")));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->serverPsn, 2U);
}

TEST(V2Wire, PrepareRejectsMalformedLine)
{
    EXPECT_FALSE(parsePrepareReply(200, prepareOkHeaders(kGoodSession, "000001") + "NoColon\r\n"));
    EXPECT_FALSE(parsePrepareReply(501, "NoColon"));
}

TEST(V2Wire, PrepareRejectsMalformedReplyHeader)
{
    for (const std::string &reply :
         {std::string("200"), std::string("200") + kGoodToken, std::string("500:") + kGoodToken,
          std::string("20:") + kGoodToken, std::string("2000:") + kGoodToken,
          std::string("200:") + kGoodToken + "0"}) {
        std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
        h += "X-Amz-Rdma-Reply: " + reply + "\r\n";
        h += std::string("X-Amz-Rdma-Session: ") + kGoodSession + "\r\n";
        h += "X-Amz-Rdma-Psn: 000001\r\n";
        EXPECT_FALSE(parsePrepareReply(200, h)) << "reply " << reply;
    }
}

TEST(V2Wire, PrepareSuccessNeedsTokenAndPsn)
{
    std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
    h += std::string("X-Amz-Rdma-Session: ") + kGoodSession + "\r\n";
    std::string withPsn   = h + "X-Amz-Rdma-Psn: 000001\r\n";
    std::string withToken = h + "X-Amz-Rdma-Reply: 200:" + kGoodToken + "\r\n";

    EXPECT_FALSE(parsePrepareReply(200, withPsn));
    EXPECT_FALSE(parsePrepareReply(200, withToken));
    EXPECT_FALSE(parsePrepareReply(200, withToken + "X-Amz-Rdma-Psn: zzzzzz\r\n"));
}

TEST(V2Wire, FinalToleratesBlankLinesAndMissingFinalCrlf)
{
    EXPECT_TRUE(parseFinalReply(200, "\r\n" + finalOkHeaders() + "\r\n"));
    const auto r = parseFinalReply(200, withoutFinalCrlf(finalOkHeaders()));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->cookieEcho, 0x00c0ffeeU);
}

TEST(V2Wire, FinalRejectsMalformedLine)
{
    EXPECT_FALSE(parseFinalReply(200, finalOkHeaders() + "NoColon\r\n"));
}

TEST(V2Wire, FinalRejectsMalformedCookie)
{
    for (const char *cookie : {"", "c0ffee", "00c0ffee0", "zzzzzzzz", "00c0ffeg", "0x00c0ff", "+0c0ffee",
                               "-0c0ffee", "00 c0ffe"}) {
        std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
        h += std::string("X-Amz-Rdma-Cookie: ") + cookie + "\r\n";
        EXPECT_FALSE(parseFinalReply(200, h)) << "cookie \"" << cookie << "\"";
    }

    /* Hex digits in either case are fine */
    const auto r =
        parseFinalReply(200, "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\nX-Amz-Rdma-Cookie: 00C0FFEE\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->cookieEcho, 0x00c0ffeeU);
}

TEST(V2Wire, FinalRejectsMalformedBytesTransferred)
{
    for (const char *bytes : {"", "12a", "-1", "+1", "1 2", "0x10"}) {
        std::string h = finalOkHeaders() + "X-Amz-Rdma-Bytes-Transferred: " + bytes + "\r\n";
        EXPECT_FALSE(parseFinalReply(200, h)) << "bytes \"" << bytes << "\"";
    }
}

TEST(V2Wire, FinalReadsVersionId)
{
    const auto r = parseFinalReply(200, finalOkHeaders() + "X-Amz-Rdma-Version-Id: v42\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->versionId, "v42");
}

TEST(V2Wire, FinalSuccessNeedsProtocolEcho)
{
    for (int status : {200, 204}) {
        EXPECT_FALSE(parseFinalReply(status, "X-Amz-Rdma-Cookie: 00c0ffee\r\n")) << "status " << status;
    }
}

/* Random 8-byte values and their base64, from Python's base64 module */
constexpr const char *kChecksumVectors[] = {
    "SgtwPfgnR/s=", /* 4a0b703df82747fb */
    "Fve3Nk/IrLU=", /* 16f7b7364fc8acb5 */
    "N2gfkb93B2Y=", /* 37681f91bf770766 */
    "XTk8Sf9zDzE=", /* 5d393c49ff730f31 */
    "F43S3Er4qGc=", /* 178dd2dc4af8a867 */
    "fnb8dg4CzFo=", /* 7e76fc760e02cc5a */
    "E8F9EFTNlIc=", /* 13c17d1054cd9487 */
    "mxFRIVnUDRQ=", /* 9b11512159d40d14 */
    "+/v7+/v7+/s=", /* fbfbfbfbfbfbfbfb */
    "//////////8=", /* ffffffffffffffff */
    "AAAAAAAAAAE=", /* 0000000000000001 */
};

TEST(V2Wire, ChecksumAcceptsCanonicalBase64)
{
    for (const char *checksum : kChecksumVectors) {
        const std::string value = std::string("CRC64NVME ") + checksum;
        EXPECT_EQ(validateChecksumText(value), checksum);

        const auto r = parseFinalReply(204, finalOkHeaders() + "X-Amz-Rdma-Checksum: " + value + "\r\n");
        ASSERT_TRUE(r) << checksum;
        EXPECT_EQ(r->checksumB64, checksum);
    }
}

TEST(V2Wire, ChecksumRejectsPaddingBits)
{
    /* The last character before the '=' carries 4 bits of the checksum and
     * 2 bits of padding, which must be zero. Setting either padding bit
     * gives text that decodes, but isn't canonical. */
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (const char *checksum : kChecksumVectors) {
        for (size_t padding : {1U, 2U, 3U}) {
            std::string text = checksum;
            size_t      v    = alphabet.find(text[10]);
            ASSERT_NE(v, std::string::npos);
            text[10] = alphabet[v | padding];
            EXPECT_FALSE(validateChecksumText("CRC64NVME " + text)) << text;
        }
    }
}

TEST(V2Wire, FinalRejectsBytesTransferredThatOverflows)
{
    auto r =
        parseFinalReply(200, finalOkHeaders() + "X-Amz-Rdma-Bytes-Transferred: 18446744073709551615\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->bytes, std::numeric_limits<uint64_t>::max());

    /* UINT64_MAX + 1, which overflows in the addition; and a value that
     * overflows in the multiplication. Neither may wrap to a small count. */
    for (const char *bytes : {"18446744073709551616", "18446744073709551620", "99999999999999999999",
                              "100000000000000000000", "184467440737095516150"}) {
        std::string h = finalOkHeaders() + "X-Amz-Rdma-Bytes-Transferred: " + bytes + "\r\n";
        EXPECT_FALSE(parseFinalReply(200, h)) << "bytes " << bytes;
    }

    /* Leading zeros don't overflow */
    r = parseFinalReply(200,
                        finalOkHeaders() + "X-Amz-Rdma-Bytes-Transferred: 0000000000000000000000042\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->bytes, 42U);
}

TEST(V2Wire, ChecksumRejectsMalformedText)
{
    /* 12 characters, but no padding */
    EXPECT_FALSE(validateChecksumText("CRC64NVME AAAAAAAAAAAA"));
    /* Not base64 */
    EXPECT_FALSE(validateChecksumText("CRC64NVME AAAAA-AAAAA="));
}

TEST(V2Wire, EnumAbiCompat)
{
    /* The v2 enum extension appends after hipObjInternalError(11). */
    EXPECT_EQ(static_cast<int>(hipObjInternalError), 11);
    EXPECT_EQ(static_cast<int>(hipObjNotSupported), 12);
    EXPECT_EQ(static_cast<int>(hipObjBusy), 13);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
