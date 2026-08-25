/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Unit tests for the hipobj-rc-v2 wire helpers (src/rdma/v2-wire.*).
 * These are pure parsing/formatting tests and need no RDMA hardware. */

#include <string>

#include <gtest/gtest.h>

#include "hipobj.h"
#include "v2-wire.h"

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

const char* kGoodSession = "0123456789abcdef0123456789abcdef";
const char* kGoodToken =
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
  "2122232425262728292a2b";

std::string prepareOkHeaders(const char* session, const char* psn) {
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += std::string("X-Amz-Rdma-Reply: 200:") + kGoodToken + "\r\n";
  h += std::string("X-Amz-Rdma-Session: ") + session + "\r\n";
  h += std::string("X-Amz-Rdma-Psn: ") + psn + "\r\n";
  return h;
}

TEST(V2Wire, SplitHeaderLine) {
  std::string n, v;
  EXPECT_TRUE(splitHeaderLine("Name: value", n, v));
  EXPECT_EQ(n, "Name");
  EXPECT_EQ(v, "value");
  EXPECT_TRUE(splitHeaderLine("  Padded  :  v  ", n, v));
  EXPECT_EQ(n, "Padded");
  EXPECT_EQ(v, "v");
  EXPECT_FALSE(splitHeaderLine("NoColon", n, v));
  EXPECT_FALSE(splitHeaderLine(": novalue", n, v));
}

TEST(V2Wire, PrepareOk) {
  hipObj::v2::PrepareReply r;
  EXPECT_TRUE(
    parsePrepareReply(200, prepareOkHeaders(kGoodSession, "01ab01"), r));
  EXPECT_TRUE(r.protocolEcho);
  EXPECT_FALSE(r.unsupportedMarker);
  EXPECT_EQ(r.serverToken, std::string(kGoodToken));
  EXPECT_EQ(r.session, std::string(kGoodSession));
  EXPECT_EQ(r.serverPsn, 0x01ab01u);
}

TEST(V2Wire, PrepareOkCaseInsensitiveHeaderNames) {
  std::string h = "x-aMz-rDmA-pRoToCoL: hipobj-rc-v2\r\n";
  h += std::string("x-amz-rdma-reply: 200:") + kGoodToken + "\r\n";
  h += std::string("X-AMZ-RDMA-SESSION: ") + kGoodSession + "\r\n";
  h += "x-amz-rdma-psn: ffffff\r\n";
  hipObj::v2::PrepareReply r;
  EXPECT_TRUE(parsePrepareReply(200, h, r));
  EXPECT_TRUE(r.protocolEcho);
  EXPECT_EQ(r.serverPsn, 0xffffffu);
}

TEST(V2Wire, PrepareUnsupported) {
  std::string h = "X-Amz-Rdma-Protocol-Status: unsupported\r\n";
  hipObj::v2::PrepareReply r;
  EXPECT_TRUE(parsePrepareReply(501, h, r));
  EXPECT_FALSE(r.protocolEcho);
  EXPECT_TRUE(r.unsupportedMarker);
}

TEST(V2Wire, PrepareBare501Rejected) {
  /* An unmarked 501 (proxy error etc.) must not look like support
   * negotiation; the marker is what makes 501 protocol-specific. */
  hipObj::v2::PrepareReply r;
  EXPECT_TRUE(parsePrepareReply(501, "", r));
  EXPECT_TRUE(!r.unsupportedMarker);
}

TEST(V2Wire, PrepareMissingEchoIsError) {
  std::string h = std::string("X-Amz-Rdma-Reply: 200:") + kGoodToken + "\r\n" +
                  std::string("X-Amz-Rdma-Session: ") + kGoodSession +
                  "\r\nX-Amz-Rdma-Psn: 000001\r\n";
  hipObj::v2::PrepareReply r;
  EXPECT_FALSE(parsePrepareReply(200, h, r));
}

TEST(V2Wire, PrepareZeroPsnIsError) {
  hipObj::v2::PrepareReply r;
  EXPECT_FALSE(
    parsePrepareReply(200, prepareOkHeaders(kGoodSession, "000000"), r));
}

TEST(V2Wire, PrepareBadSessionRejected) {
  hipObj::v2::PrepareReply r;
  EXPECT_FALSE(parsePrepareReply(200, prepareOkHeaders("0123", "000001"), r));
  EXPECT_FALSE(parsePrepareReply(
    200, prepareOkHeaders("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "000001"), r));
}

TEST(V2Wire, PrepareBadTokenRejected) {
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += "X-Amz-Rdma-Reply: 200:tooshort\r\n";
  h += std::string("X-Amz-Rdma-Session: ") + kGoodSession + "\r\n";
  h += "X-Amz-Rdma-Psn: 000001\r\n";
  hipObj::v2::PrepareReply r;
  EXPECT_FALSE(parsePrepareReply(200, h, r));
}

TEST(V2Wire, FinalGetOk) {
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += "X-Amz-Rdma-Cookie: 00c0ffee\r\n";
  h += "X-Amz-Rdma-Bytes-Transferred: 65536\r\n";
  h += "X-Amz-Rdma-Etag: \"abc123\"\r\n";
  hipObj::v2::FinalReply r;
  EXPECT_TRUE(parseFinalReply(200, h, r));
  EXPECT_TRUE(r.protocolEcho);
  EXPECT_EQ(r.cookieEcho, 0x00c0ffeeu);
  EXPECT_EQ(r.bytes, 65536u);
  EXPECT_EQ(r.etag, "\"abc123\"");
}

TEST(V2Wire, FinalPutOkWithChecksum) {
  /* CRC64NVME of the empty string, canonical base64: 8 bytes -> 12 chars
   * ending with '='. Use a known-vector style constant; the parser only
   * validates canonical form. */
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += "X-Amz-Rdma-Cookie: deadbeef\r\n";
  h += "X-Amz-Rdma-Bytes-Transferred: 1024\r\n";
  h += "X-Amz-Rdma-Checksum: CRC64NVME AAAAAAAAAAA=\r\n";
  hipObj::v2::FinalReply r;
  EXPECT_TRUE(parseFinalReply(204, h, r));
  EXPECT_EQ(r.checksumB64, "AAAAAAAAAAA=");
}

TEST(V2Wire, FinalChecksumNonCanonicalRejected) {
  /* Non-canonical: last data char has nonzero pad bits. */
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += "X-Amz-Rdma-Cookie: deadbeef\r\n";
  h += "X-Amz-Rdma-Bytes-Transferred: 1024\r\n";
  h += "X-Amz-Rdma-Checksum: CRC64NVME AAAAAAAAAAB=\r\n";
  hipObj::v2::FinalReply r;
  EXPECT_FALSE(parseFinalReply(204, h, r));
}

TEST(V2Wire, FinalChecksumBadLengthRejected) {
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += "X-Amz-Rdma-Cookie: deadbeef\r\n";
  h += "X-Amz-Rdma-Checksum: CRC64NVME AAAA=\r\n";
  hipObj::v2::FinalReply r;
  EXPECT_FALSE(parseFinalReply(204, h, r));
}

TEST(V2Wire, FinalMissingCookieRejected) {
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  h += "X-Amz-Rdma-Bytes-Transferred: 1\r\n";
  hipObj::v2::FinalReply r;
  EXPECT_FALSE(parseFinalReply(200, h, r));
}

TEST(V2Wire, FinalErrHasNoMandatoryCookie) {
  std::string h = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n";
  hipObj::v2::FinalReply r;
  EXPECT_TRUE(parseFinalReply(500, h, r));
  EXPECT_FALSE(r.protocolEcho == false);
}

TEST(V2Wire, ChecksumTextValidation) {
  std::string out;
  EXPECT_TRUE(validateChecksumText("CRC64NVME AAAAAAAAAAA=", out));
  EXPECT_EQ(out, "AAAAAAAAAAA=");
  EXPECT_FALSE(validateChecksumText("CRC64NV AAAAAAAAAAA=", out));
  EXPECT_FALSE(validateChecksumText("CRC64NVME AAAAAAAAAAA", out));
  EXPECT_FALSE(validateChecksumText("CRC64NVME AAAAAAAAAA==", out));
  EXPECT_FALSE(validateChecksumText("CRC64NVME =AAAAAAAAAA=", out));
}

TEST(V2Wire, SessionHex) {
  EXPECT_TRUE(isValidSessionHex(kGoodSession));
  EXPECT_TRUE(isValidSessionHex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));
  EXPECT_FALSE(isValidSessionHex("short"));
  EXPECT_FALSE(isValidSessionHex("g123456789abcdef0123456789abcdef"));
}

TEST(V2Wire, Psn) {
  uint32_t v = 0;
  EXPECT_TRUE(parsePsn("000001", v));
  EXPECT_EQ(v, 1u);
  EXPECT_TRUE(parsePsn("ffffff", v));
  EXPECT_EQ(v, 0xffffffu);
  EXPECT_FALSE(parsePsn("000000", v));
  EXPECT_FALSE(parsePsn("1000000", v));
  EXPECT_FALSE(parsePsn("zzzzzz", v));
}

TEST(V2Wire, FormatHelpers) {
  EXPECT_EQ(formatCookie(0xdeadbeefu), "deadbeef");
  EXPECT_EQ(formatPsn(1), "000001");
  EXPECT_EQ(formatPsn(0xffffffu), "ffffff");
}

TEST(V2Wire, BuildTarget) {
  EXPECT_EQ(buildTarget("b", "k", ""), "/b/k");
  EXPECT_EQ(buildTarget("bucket", "key name", ""), "/bucket/key%20name");
  EXPECT_EQ(buildTarget("b", "k", "partNumber=1"), "/b/k?partNumber=1");
  EXPECT_EQ(buildTarget("b", "a/b", ""), "/b/a/b");
}

TEST(V2Wire, EnumAbiCompat) {
  /* The v2 enum extension appends after hipObjInternalError(11). */
  EXPECT_EQ((int)hipObjInternalError, 11);
  EXPECT_EQ((int)hipObjNotSupported, 12);
  EXPECT_EQ((int)hipObjBusy, 13);
}

} // namespace
