/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding implementation */

#include "token.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <netinet/in.h>

#include "hipobj-parse.h"

namespace hipObj {

namespace {

    constexpr std::string_view kHexDigits = "0123456789abcdef";

    /* The token's binary form: its fields, in the order docs/interop.rst
     * lists them, with no padding */
    constexpr size_t kTokenBinaryLen = sizeof(RdmaToken::transport) + sizeof(RdmaToken::qpNum) + kGidLen +
                                       sizeof(RdmaToken::rkey) + sizeof(RdmaToken::remoteAddr) +
                                       sizeof(RdmaToken::length) + sizeof(RdmaToken::portNum) +
                                       sizeof(RdmaToken::lid);
    /* Fails when a field of RdmaToken changes size, which changes the wire
     * format */
    static_assert(kRdmaTokenHexLen == kTokenBinaryLen * 2);

    /* Writes a token's binary form front to back, with its integers
     * little-endian on any host */
    class TokenWriter {
    public:
        explicit TokenWriter(std::span<uint8_t> out) : rest_(out)
        {
        }

        template <std::unsigned_integral T> void put(T value)
        {
            for (size_t i = 0; i < sizeof(T); ++i)
                rest_[i] = static_cast<uint8_t>(value >> (8 * i));
            rest_ = rest_.subspan(sizeof(T));
        }

        void put(std::span<const uint8_t> bytes)
        {
            std::ranges::copy(bytes, rest_.begin());
            rest_ = rest_.subspan(bytes.size());
        }

    private:
        std::span<uint8_t> rest_;
    };

    /* Reads a token's binary form front to back, the way TokenWriter
     * writes it */
    class TokenReader {
    public:
        explicit TokenReader(std::span<const uint8_t> in) : rest_(in)
        {
        }

        template <std::unsigned_integral T> T get()
        {
            T value = 0;
            /* Not |=, which -Wconversion flags for a uint16_t, since the
             * operands are promoted to int */
            for (size_t i = 0; i < sizeof(T); ++i)
                value = static_cast<T>(value | (T{rest_[i]} << (8 * i)));
            rest_ = rest_.subspan(sizeof(T));
            return value;
        }

        void get(std::span<uint8_t> bytes)
        {
            std::ranges::copy(rest_.first(bytes.size()), bytes.begin());
            rest_ = rest_.subspan(bytes.size());
        }

    private:
        std::span<const uint8_t> rest_;
    };

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
    std::array<uint8_t, kTokenBinaryLen> buf{};
    TokenWriter                          out(buf);
    out.put(static_cast<uint8_t>(token.transport));
    out.put(token.qpNum);
    out.put(token.gid);
    out.put(token.rkey);
    out.put(token.remoteAddr);
    out.put(token.length);
    out.put(token.portNum);
    out.put(token.lid);

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

    std::array<uint8_t, kTokenBinaryLen> buf{};
    for (size_t i = 0; i < kTokenBinaryLen; ++i) {
        const auto byte = parseNumber<uint8_t>(tokenHex.substr(i * 2, 2), 16);
        if (!byte)
            return std::nullopt;
        buf[i] = *byte;
    }

    TokenReader in(buf);
    /* Any uint8_t is a valid TransportType value, so the cast is
     * defined even for a byte that names no enumerator */
    const auto transport = static_cast<TransportType>(in.get<uint8_t>());
    if (transport != TransportType::DC && transport != TransportType::RC)
        return std::nullopt;

    RdmaToken token;
    token.transport = transport;
    token.qpNum     = in.get<uint32_t>();
    in.get(token.gid);
    token.rkey       = in.get<uint32_t>();
    token.remoteAddr = in.get<uint64_t>();
    token.length     = in.get<uint64_t>();
    token.portNum    = in.get<uint8_t>();
    token.lid        = in.get<uint16_t>();
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
