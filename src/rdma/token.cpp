/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding implementation */

#include "token.h"

#include <algorithm>
#include <array>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <netinet/in.h>

#include "hipobj-parse.h"
#include "hipobj.h"

namespace hipObj {

namespace {

    constexpr std::string_view kHexDigits = "0123456789abcdef";

    /* The low four bits of a byte, which the second of its hex digits
     * stands for */
    constexpr uint8_t kLowNibble = 0x0f;

    /* The HTTP statuses a reply's RDMA transfer succeeded with */
    constexpr int kHttpOk             = 200;
    constexpr int kHttpNoContent      = 204;
    constexpr int kHttpPartialContent = 206;

    /* The range of HTTP status codes */
    constexpr unsigned kHttpStatusMin = 100;
    constexpr unsigned kHttpStatusMax = 599;

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
            for (size_t i = 0; i < sizeof(T); ++i) {
                rest_[i] = static_cast<uint8_t>(value >> (CHAR_BIT * i));
            }
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
            for (size_t i = 0; i < sizeof(T); ++i) {
                value = static_cast<T>(value | (T{rest_[i]} << (CHAR_BIT * i)));
            }
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
        return code == kHttpOk || code == kHttpNoContent || code == kHttpPartialContent;
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

        if (value == "ok") {
            return ParsedReply{.httpCode = kHttpOk, .peerToken = std::nullopt};
        }
        if (value == "err") {
            return ParsedReply{.httpCode = -1, .peerToken = std::nullopt};
        }

        const std::string_view status = value.substr(0, value.find(':'));
        if (status.size() != 3) {
            return std::nullopt;
        }
        /* Unsigned, so a '-' sign is rejected too */
        const auto parsed = parseNumber<unsigned>(status);
        if (!parsed || *parsed < kHttpStatusMin || *parsed > kHttpStatusMax) {
            return std::nullopt;
        }
        const int code = static_cast<int>(*parsed);

        if (status.size() == value.size()) {
            return ParsedReply{.httpCode = code, .peerToken = std::nullopt};
        }
        /* Only a successful reply carries a peer token */
        if (code != kHttpOk) {
            return std::nullopt;
        }
        const auto token = decodeRdmaTokenHex(value.substr(status.size() + 1));
        if (!token) {
            return std::nullopt;
        }
        return ParsedReply{.httpCode = code, .peerToken = token};
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
        hex[i * 2]       = kHexDigits[buf[i] >> 4];
        hex[(i * 2) + 1] = kHexDigits[buf[i] & kLowNibble];
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
    if (tokenHex.size() != kRdmaTokenHexLen) {
        return std::nullopt;
    }

    std::array<uint8_t, kTokenBinaryLen> buf{};
    for (size_t i = 0; i < kTokenBinaryLen; ++i) {
        const auto byte = parseNumber<uint8_t>(tokenHex.substr(i * 2, 2), 16);
        if (!byte) {
            return std::nullopt;
        }
        buf[i] = *byte;
    }

    TokenReader in(buf);
    /* Any uint8_t is a valid TransportType value, so the cast is
     * defined even for a byte that names no enumerator */
    const auto transport = static_cast<TransportType>(in.get<uint8_t>());
    if (transport != TransportType::DC && transport != TransportType::RC) {
        return std::nullopt;
    }

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
    if (!parsed) {
        return std::nullopt;
    }
    return parsed->httpCode;
}

std::optional<RdmaReplyStatus>
decodeRdmaReply(std::string_view reply)
{
    const auto httpCode = parseRdmaReplyHttpCode(reply);
    if (!httpCode) {
        return std::nullopt;
    }
    if (*httpCode == HIPOBJ_RDMA_REPLY_NOT_IMPLEMENTED) {
        return RdmaReplyStatus::NotSupported;
    }
    if (isSuccessHttpCode(*httpCode)) {
        return RdmaReplyStatus::Success;
    }
    return RdmaReplyStatus::Failed;
}

bool
parseClientNicFromTokenHex(std::string_view tokenHex, std::span<char> nicIp)
{
    if (nicIp.empty()) {
        return false;
    }
    nicIp[0] = '\0';

    const auto token = decodeRdmaTokenHex(tokenHex);
    if (!token) {
        return false;
    }

    if (!isIpv4MappedGid(token->gid)) {
        return true;
    }

    /* Formats into a buffer that holds any IPv4 address and its NUL, then
     * copies that only if nicIp, whose size is caller-supplied, has room.
     * format_to_n() doesn't write a NUL, so it gets one less than the
     * buffer's size, and returns the length of the whole address. The
     * bytes are uint8_t, which std::format prints as numbers, not
     * characters. */
    const std::span<const uint8_t, 4> ipv4 = ipv4OfGid(token->gid);
    std::array<char, INET_ADDRSTRLEN> buf{};
    const auto                        formatted =
        std::format_to_n(buf.begin(), buf.size() - 1, "{}.{}.{}.{}", ipv4[0], ipv4[1], ipv4[2], ipv4[3]);
    const auto length = static_cast<size_t>(formatted.size);
    if (length >= nicIp.size()) {
        return false;
    }
    std::ranges::copy(std::span(buf).first(length + 1), nicIp.begin());
    return true;
}

std::optional<RdmaToken>
parsePeerTokenFromReply(std::string_view reply)
{
    const auto parsed = parseReply(reply);
    if (!parsed) {
        return std::nullopt;
    }
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
