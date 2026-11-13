/* SPDX-License-Identifier: MIT
 */

#include "v3-wire.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "gid.h"
#include "hipobj-parse.h"
#include "hipobj-span.h"

namespace hipObj::v3 {

namespace {

    /* The kFieldDelim-separated fields of `s`, or std::nullopt unless there are
     * exactly N: fewer is a missing field, more is input the decoder would
     * otherwise ignore. */
    template <size_t N> std::optional<std::array<std::string_view, N>> splitExactly(std::string_view s)
    {
        std::array<std::string_view, N> fields{};
        for (size_t i = 0; i + 1 < N; ++i) {
            const size_t delim = s.find(kFieldDelim);
            if (delim == std::string_view::npos) {
                return std::nullopt;
            }
            fields[i] = s.substr(0, delim);
            s.remove_prefix(delim + 1);
        }
        if (s.find(kFieldDelim) != std::string_view::npos) {
            return std::nullopt;
        }
        fields[N - 1] = s;
        return fields;
    }

    /* The widths of the token's fields after the ip field, and of the
     * reply's byte count, in hex digits (see the file comment) */
    constexpr size_t kPortHexDigits   = 4;
    constexpr size_t kRkeyHexDigits   = 8;
    constexpr size_t kU64HexDigits    = 16;
    constexpr int    kHexBase         = 16;
    constexpr size_t kBitsPerHexDigit = 4;

    /* The range of HTTP status codes */
    constexpr unsigned kMinHttpStatus = 100;
    constexpr unsigned kMaxHttpStatus = 599;

    /* Exactly `width` hex digits as a T. parseNumber() rejects a sign,
     * whitespace and a "0x" prefix, and a value that doesn't fit in T. */
    template <typename T> std::optional<T> hexField(std::string_view s, size_t width)
    {
        if (s.size() != width) {
            return std::nullopt;
        }
        return parseNumber<T>(s, kHexBase);
    }

    /* The 16 bytes of a token's ip field, in network order: a GID's size */
    using IpBytes = std::array<uint8_t, kGidLen>;

    /* The 16 bytes of the token's ip field: exactly 32 hex digits */
    std::optional<IpBytes> ipBytesField(std::string_view s)
    {
        IpBytes bytes{};
        if (s.size() != 2 * bytes.size()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < bytes.size(); ++i) {
            const auto byte = hexField<uint8_t>(s.substr(2 * i, 2), 2);
            if (!byte) {
                return std::nullopt;
            }
            bytes[i] = *byte;
        }
        return bytes;
    }

    /* Writes value's low `width` hex digits, lowercase and zero-padded, at the
     * front of out, and returns the rest of out */
    std::span<char> putHex(std::span<char> out, uint64_t value, size_t width)
    {
        static constexpr std::string_view kDigits = "0123456789abcdef";
        for (size_t i = 0; i < width; ++i) {
            const size_t shift = kBitsPerHexDigit * (width - 1 - i);
            out[i]             = kDigits[(value >> shift) % kDigits.size()];
        }
        return out.subspan(width);
    }

    /* Writes c at the front of out, and returns the rest of out */
    std::span<char> putChar(std::span<char> out, char c)
    {
        out[0] = c;
        return out.subspan(1);
    }

    /* The bytes of an IPv4 address */
    using V4Bytes = std::array<uint8_t, sizeof(in_addr)>;

    /* token.listenIp (empty, dotted-quad IPv4, or colon-form IPv6) -> 16 raw
     * bytes in network order. Empty/unparsable -> all-zero ("::", unspecified).
     */
    IpBytes ipStringToBytes(const std::string &ip)
    {
        IpBytes out{};
        if (ip.empty()) {
            return out;
        }
        in_addr v4{};
        if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
            const std::span<uint8_t> bytes(out);
            std::ranges::copy(kIpv4MappedPrefix, bytes.begin());
            std::ranges::copy(std::bit_cast<V4Bytes>(v4), bytes.subspan(kIpv4MappedPrefix.size()).begin());
            return out;
        }
        in6_addr v6{};
        if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) {
            out = std::bit_cast<IpBytes>(v6);
        }
        return out;
    }

    /* Reverse of ipStringToBytes(): 16 raw bytes -> dotted-quad IPv4 (if
     * IPv4-mapped), colon-form IPv6, or empty (all-zero). */
    bool bytesToIpString(const IpBytes &in, std::string &ip)
    {
        if (std::ranges::all_of(in, [](uint8_t byte) noexcept { return byte == 0; })) {
            ip.clear();
            return true;
        }
        std::array<char, INET6_ADDRSTRLEN> buf{};
        const std::span<const uint8_t>     bytes(in);
        if (std::ranges::equal(bytes.first(kIpv4MappedPrefix.size()), kIpv4MappedPrefix)) {
            V4Bytes v4Bytes{};
            std::ranges::copy(bytes.subspan(kIpv4MappedPrefix.size()), v4Bytes.begin());
            const auto v4 = std::bit_cast<in_addr>(v4Bytes);
            if (inet_ntop(AF_INET, &v4, buf.data(), static_cast<socklen_t>(buf.size())) == nullptr) {
                return false;
            }
        }
        else {
            const auto v6 = std::bit_cast<in6_addr>(in);
            if (inet_ntop(AF_INET6, &v6, buf.data(), static_cast<socklen_t>(buf.size())) == nullptr) {
                return false;
            }
        }
        ip = hipObj::cStringView(buf);
        return true;
    }

} // namespace

RdmaTokenV3Hex
encodeRdmaTokenV3Hex(const RdmaTokenV3 &token)
{
    /* Every field is fixed-width, so the result is always exactly
     * kRdmaTokenV3HexLen characters and fits with its NUL. Zeroed first, so
     * nothing but the token's own characters is ever left in the array. */
    RdmaTokenV3Hex  out{};
    std::span<char> rest(out);
    for (const uint8_t byte : ipStringToBytes(token.listenIp)) {
        rest = putHex(rest, byte, 2);
    }
    rest = putChar(rest, kFieldDelim);
    rest = putHex(rest, token.listenPort, kPortHexDigits);
    rest = putChar(rest, kFieldDelim);
    rest = putHex(rest, token.rkey, kRkeyHexDigits);
    rest = putChar(rest, kFieldDelim);
    rest = putHex(rest, token.remoteAddr, kU64HexDigits);
    rest = putChar(rest, kFieldDelim);
    putHex(rest, token.length, kU64HexDigits);
    return out;
}

std::optional<RdmaTokenV3>
decodeRdmaTokenV3(std::string_view headerValue)
{
    /* ip-hex32 : port-hex4 : rkey-hex8 : remoteAddr-hex16 : length-hex16 --
     * 5 fields, all hex, kFieldDelim-delimited (see the file comment). */
    if (headerValue.size() != kRdmaTokenV3HexLen) {
        return std::nullopt;
    }
    const auto fields = splitExactly<5>(headerValue);
    if (!fields) {
        return std::nullopt;
    }
    const auto ipBytes    = ipBytesField((*fields)[0]);
    const auto port       = hexField<uint16_t>((*fields)[1], kPortHexDigits);
    const auto rkey       = hexField<uint32_t>((*fields)[2], kRkeyHexDigits);
    const auto remoteAddr = hexField<uint64_t>((*fields)[3], kU64HexDigits);
    const auto length     = hexField<uint64_t>((*fields)[4], kU64HexDigits);
    if (!ipBytes || !port || !rkey || !remoteAddr || !length) {
        return std::nullopt;
    }

    RdmaTokenV3 token;
    if (!bytesToIpString(*ipBytes, token.listenIp)) {
        return std::nullopt;
    }
    token.listenPort = *port;
    token.rkey       = *rkey;
    token.remoteAddr = *remoteAddr;
    token.length     = *length;
    return token;
}

std::string
encodeRdmaReplyV3(const RdmaReplyV3 &reply)
{
    std::array<char, kU64HexDigits> bytesHex{};
    putHex(bytesHex, reply.bytesTransferred, bytesHex.size());
    std::string out = std::to_string(reply.httpStatus);
    out.push_back(kFieldDelim);
    std::ranges::copy(bytesHex, std::back_inserter(out));
    return out;
}

std::optional<RdmaReplyV3>
decodeRdmaReplyV3(std::string_view replyValue)
{
    /* A callback may report trailing NUL, CR and LF characters; v1's reply
     * parser (token.cpp) ignores them too */
    while (!replyValue.empty() &&
           (replyValue.back() == '\0' || replyValue.back() == '\n' || replyValue.back() == '\r')) {
        replyValue.remove_suffix(1);
    }
    /* httpStatus-decimal3 : bytesTransferred-hex16 */
    const auto fields = splitExactly<2>(replyValue);
    if (!fields) {
        return std::nullopt;
    }
    /* Exactly 3 digits, as HTTP status codes are; unsigned, so a sign is
     * rejected too */
    const std::string_view statusField = (*fields)[0];
    const auto status = statusField.size() == 3 ? parseNumber<unsigned>(statusField) : std::nullopt;
    if (!status || *status < kMinHttpStatus || *status > kMaxHttpStatus) {
        return std::nullopt;
    }
    const auto bytesTransferred = hexField<uint64_t>((*fields)[1], kU64HexDigits);
    if (!bytesTransferred) {
        return std::nullopt;
    }
    RdmaReplyV3 reply;
    reply.httpStatus       = static_cast<int>(*status);
    reply.bytesTransferred = *bytesTransferred;
    return reply;
}

} // namespace hipObj::v3
