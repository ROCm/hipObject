/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding implementation */

#include "token.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <netinet/in.h>

#include "hipobj-parse.h"

namespace hipObj {

namespace {

    constexpr std::string_view kHexDigits = "0123456789abcdef";

    constexpr size_t kTokenBinaryLen = 1 + 4 + kGidLen + 4 + 8 + 8 + 1 + 2;
    static_assert(kRdmaTokenHexLen == kTokenBinaryLen * 2);

    bool isSuccessHttpCode(int code)
    {
        return code == 200 || code == 204 || code == 206;
    }

    /* An x-amz-rdma-reply value: its HTTP status (200 for "ok", -1 for
     * "err"), and the peer token if it carries one */
    struct ParsedReply {
        int                      httpCode = 0;
        std::optional<RdmaToken> peerToken;
    };

    /* Parses an x-amz-rdma-reply value in one of the forms docs/interop.rst
     * lists: "ok", "err", a three-digit HTTP status, or "200:" followed by
     * an 88-hex peer token. Trailing NUL, CR, and LF characters are
     * ignored; anything else that doesn't match is rejected. */
    std::optional<ParsedReply> parseReply(std::string_view reply)
    {
        std::string_view value = reply;
        while (!value.empty() && (value.back() == '\0' || value.back() == '\n' || value.back() == '\r')) {
            value.remove_suffix(1);
        }

        if (value == "ok")
            return ParsedReply{200, std::nullopt};
        if (value == "err")
            return ParsedReply{-1, std::nullopt};

        const std::string_view status = value.substr(0, value.find(':'));
        if (status.size() != 3)
            return std::nullopt;
        /* Unsigned, so a '-' sign is rejected too */
        const auto parsed = parseNumber<unsigned>(status);
        if (!parsed || *parsed < 100 || *parsed > 599)
            return std::nullopt;
        const int code = static_cast<int>(*parsed);

        if (status.size() == value.size())
            return ParsedReply{code, std::nullopt};
        /* Only a successful reply carries a peer token */
        if (code != 200)
            return std::nullopt;
        auto token = decodeRdmaTokenHex(value.substr(status.size() + 1));
        if (!token)
            return std::nullopt;
        return ParsedReply{code, token};
    }

} // namespace

RdmaTokenHex
encodeRdmaTokenHex(const RdmaToken &token)
{
    uint8_t buf[kTokenBinaryLen];
    size_t  off = 0;

    buf[off++] = static_cast<uint8_t>(token.transport);
    std::memcpy(buf + off, &token.qpNum, 4);
    off += 4;
    std::memcpy(buf + off, token.gid.data(), token.gid.size());
    off += token.gid.size();
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

std::optional<RdmaToken>
decodeRdmaTokenHex(std::string_view tokenHex)
{
    if (tokenHex.size() != kRdmaTokenHexLen)
        return std::nullopt;

    uint8_t buf[kTokenBinaryLen];
    for (size_t i = 0; i < kTokenBinaryLen; ++i) {
        const auto byte = parseNumber<uint8_t>(tokenHex.substr(i * 2, 2), 16);
        if (!byte)
            return std::nullopt;
        buf[i] = *byte;
    }
    /* Any uint8_t is a valid TransportType value, so the cast is
     * defined even for a byte that names no enumerator */
    const auto transport = static_cast<TransportType>(buf[0]);
    if (transport != TransportType::DC && transport != TransportType::RC)
        return std::nullopt;

    RdmaToken token;
    size_t    off   = 1;
    token.transport = transport;
    std::memcpy(&token.qpNum, buf + off, 4);
    off += 4;
    std::memcpy(token.gid.data(), buf + off, token.gid.size());
    off += token.gid.size();
    std::memcpy(&token.rkey, buf + off, 4);
    off += 4;
    std::memcpy(&token.remoteAddr, buf + off, 8);
    off += 8;
    std::memcpy(&token.length, buf + off, 8);
    off += 8;
    token.portNum = buf[off++];
    std::memcpy(&token.lid, buf + off, 2);
    return token;
}

std::optional<int>
parseRdmaReplyHttpCode(std::string_view reply)
{
    const auto parsed = parseReply(reply);
    if (!parsed)
        return std::nullopt;
    return parsed->httpCode;
}

std::optional<RdmaReplyStatus>
decodeRdmaReply(std::string_view reply)
{
    const auto httpCode = parseRdmaReplyHttpCode(reply);
    if (!httpCode)
        return std::nullopt;
    if (*httpCode == 501)
        return RdmaReplyStatus::NotSupported;
    if (isSuccessHttpCode(*httpCode))
        return RdmaReplyStatus::Success;
    return RdmaReplyStatus::Failed;
}

bool
parseClientNicFromTokenHex(const char *tokenHex, char *nicIp, size_t nicIpLen)
{
    if (!nicIp || nicIpLen == 0)
        return false;
    nicIp[0] = '\0';
    if (!tokenHex)
        return false;

    /* Bound the scan: anything longer than a token is rejected anyway */
    const auto token =
        decodeRdmaTokenHex(std::string_view(tokenHex, strnlen(tokenHex, kRdmaTokenHexLen + 1)));
    if (!token)
        return false;

    if (token->gid[10] != 0xff || token->gid[11] != 0xff)
        return true;

    /* Format into a fixed-size buffer first so the snprintf output is
     * bounded; nicIpLen is caller-supplied. */
    char buf[INET_ADDRSTRLEN];
    int  n = std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(token->gid[12]),
                           static_cast<unsigned>(token->gid[13]), static_cast<unsigned>(token->gid[14]),
                           static_cast<unsigned>(token->gid[15]));
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

std::optional<RdmaToken>
parsePeerTokenFromReply(std::string_view reply)
{
    const auto parsed = parseReply(reply);
    if (!parsed)
        return std::nullopt;
    return parsed->peerToken;
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
