/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding implementation */

#include "token.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <netinet/in.h>

namespace hipObj {

namespace {

    constexpr std::string_view kHexDigits = "0123456789abcdef";

    constexpr size_t kTokenBinaryLen = 1 + 4 + 16 + 4 + 8 + 8 + 1 + 2;
    static_assert(kRdmaTokenHexLen == kTokenBinaryLen * 2);

    int hexNibble(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    bool decodeHexBytePair(char hi, char lo, uint8_t &out)
    {
        int h = hexNibble(hi);
        int l = hexNibble(lo);
        if (h < 0 || l < 0)
            return false;
        out = static_cast<uint8_t>((h << 4) | l);
        return true;
    }

    bool isSuccessHttpCode(int code)
    {
        return code == 200 || code == 204 || code == 206;
    }

    /* Decodes exactly kRdmaTokenHexLen hex characters. The transport byte
     * must be one of the TransportType values. */
    bool decodeTokenHex(std::string_view tokenHex, RdmaToken &out)
    {
        if (tokenHex.size() != kRdmaTokenHexLen)
            return false;

        uint8_t buf[kTokenBinaryLen];
        for (size_t i = 0; i < kTokenBinaryLen; ++i) {
            if (!decodeHexBytePair(tokenHex[i * 2], tokenHex[i * 2 + 1], buf[i]))
                return false;
        }
        if (buf[0] != TRANSPORT_DC && buf[0] != TRANSPORT_RC)
            return false;

        size_t off    = 0;
        out.transport = buf[off++];
        std::memcpy(&out.qpNum, buf + off, 4);
        off += 4;
        std::memcpy(out.gid, buf + off, 16);
        off += 16;
        std::memcpy(&out.rkey, buf + off, 4);
        off += 4;
        std::memcpy(&out.remoteAddr, buf + off, 8);
        off += 8;
        std::memcpy(&out.length, buf + off, 8);
        off += 8;
        out.portNum = buf[off++];
        std::memcpy(&out.lid, buf + off, 2);
        return true;
    }

    /* Parses an x-amz-rdma-reply value in one of the forms docs/interop.rst
     * lists: "ok", "err", a three-digit HTTP status, or "200:" followed by
     * an 88-hex peer token. Trailing NUL, CR, and LF characters are
     * ignored; anything else that doesn't match is rejected. The outputs are
     * only written on success, and hasPeerToken says whether peerToken was. */
    bool parseReply(const char *reply, size_t replyLen, int &httpCode, RdmaToken &peerToken,
                    bool &hasPeerToken)
    {
        if (!reply)
            return false;

        size_t len = replyLen;
        while (len > 0 && (reply[len - 1] == '\0' || reply[len - 1] == '\n' || reply[len - 1] == '\r')) {
            --len;
        }
        const std::string_view value(reply, len);

        if (value == "ok") {
            httpCode     = 200;
            hasPeerToken = false;
            return true;
        }
        if (value == "err") {
            httpCode     = -1;
            hasPeerToken = false;
            return true;
        }

        const std::string_view status = value.substr(0, value.find(':'));
        if (status.size() != 3)
            return false;
        int code = 0;
        for (char c : status) {
            if (c < '0' || c > '9')
                return false;
            code = code * 10 + (c - '0');
        }
        if (code < 100 || code > 599)
            return false;

        if (status.size() == value.size()) {
            httpCode     = code;
            hasPeerToken = false;
            return true;
        }
        /* Only a successful reply carries a peer token */
        if (code != 200)
            return false;
        RdmaToken token;
        if (!decodeTokenHex(value.substr(status.size() + 1), token))
            return false;
        httpCode     = code;
        peerToken    = token;
        hasPeerToken = true;
        return true;
    }

} // namespace

RdmaTokenHex
encodeRdmaTokenHex(const RdmaToken &token)
{
    uint8_t buf[kTokenBinaryLen];
    size_t  off = 0;

    buf[off++] = token.transport;
    std::memcpy(buf + off, &token.qpNum, 4);
    off += 4;
    std::memcpy(buf + off, token.gid, 16);
    off += 16;
    std::memcpy(buf + off, &token.rkey, 4);
    off += 4;
    std::memcpy(buf + off, &token.remoteAddr, 8);
    off += 8;
    std::memcpy(buf + off, &token.length, 8);
    off += 8;
    buf[off++] = token.portNum;
    std::memcpy(buf + off, &token.lid, 2);

    RdmaTokenHex hex{}; /* zeroed, so it ends with a NUL */
    for (size_t i = 0; i < kTokenBinaryLen; ++i) {
        hex[i * 2]     = kHexDigits[buf[i] >> 4];
        hex[i * 2 + 1] = kHexDigits[buf[i] & 0xf];
    }
    return hex;
}

std::string
encodeRdmaToken(const RdmaToken &token)
{
    const RdmaTokenHex hex = encodeRdmaTokenHex(token);
    return {hex.data(), kRdmaTokenHexLen};
}

bool
decodeRdmaTokenHex(const char *tokenHex, RdmaToken &out)
{
    if (!tokenHex)
        return false;
    /* Bound the scan: anything longer than a token is rejected anyway */
    return decodeTokenHex(std::string_view(tokenHex, strnlen(tokenHex, kRdmaTokenHexLen + 1)), out);
}

bool
parseRdmaReplyHttpCode(const char *reply, size_t replyLen, int &httpCode)
{
    RdmaToken peerToken;
    bool      hasPeerToken = false;
    return parseReply(reply, replyLen, httpCode, peerToken, hasPeerToken);
}

bool
decodeRdmaReply(const char *reply, size_t replyLen, int &status)
{
    int httpCode = 0;
    if (!parseRdmaReplyHttpCode(reply, replyLen, httpCode))
        return false;
    if (httpCode == 501) {
        status = -2;
        return true;
    }
    if (httpCode < 0) {
        status = -1;
        return true;
    }
    if (isSuccessHttpCode(httpCode)) {
        status = 0;
        return true;
    }
    status = -1;
    return true;
}

bool
parseClientNicFromTokenHex(const char *tokenHex, char *nicIp, size_t nicIpLen)
{
    if (!nicIp || nicIpLen == 0)
        return false;
    nicIp[0] = '\0';
    if (!tokenHex)
        return false;

    RdmaToken token;
    if (!decodeRdmaTokenHex(tokenHex, token))
        return false;

    if (token.gid[10] != 0xff || token.gid[11] != 0xff)
        return true;

    /* Format into a fixed-size buffer first so the snprintf output is
     * bounded; nicIpLen is caller-supplied. */
    char buf[INET_ADDRSTRLEN];
    int  n = std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(token.gid[12]),
                           static_cast<unsigned>(token.gid[13]), static_cast<unsigned>(token.gid[14]),
                           static_cast<unsigned>(token.gid[15]));
    if (n < 0 || static_cast<size_t>(n) >= nicIpLen)
        return false;
    std::memcpy(nicIp, buf, static_cast<size_t>(n) + 1);
    return true;
}

std::string
formatRdmaHeaderValue(const char *tokenHex, const void *buf, size_t size)
{
    (void)buf;
    (void)size;
    return tokenHex ? std::string(tokenHex) : std::string();
}

bool
parsePeerTokenFromReply(const char *reply, size_t replyLen, RdmaToken &peerToken, int &httpCode)
{
    int       code = 0;
    RdmaToken token;
    bool      hasPeerToken = false;
    if (!parseReply(reply, replyLen, code, token, hasPeerToken) || !hasPeerToken)
        return false;
    peerToken = token;
    httpCode  = code;
    return true;
}

std::string
encodeReplyWithPeerToken(int httpCode, const RdmaToken &peerToken)
{
    const RdmaTokenHex hex   = encodeRdmaTokenHex(peerToken);
    std::string        reply = std::to_string(httpCode);
    reply += ':';
    reply.append(hex.data(), kRdmaTokenHexLen);
    return reply;
}

} // namespace hipObj
