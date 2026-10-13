/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Implementation of the hipobj-rc-v2 wire helpers (see v2-wire.h). */

#include "v2-wire.h"

#include <algorithm>
#include <optional>

#include "hipobj-format.h"
#include "hipobj-parse.h"

namespace hipObj {
namespace v2 {

    namespace {

        bool isHexDigit(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        bool isBase64Char(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
                   c == '/';
        }

        /* RFC 4648 base64 decoding table for canonical validation. */
        int b64Val(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        }

        /* Minimal strict decode/re-encode used to verify canonical text. Only
         * the 12-char (8 byte) shape is accepted here. */
        bool canonicalBase64_8Bytes(std::string_view text)
        {
            if (text.size() != kChecksumB64Len)
                return false;
            if (text.back() != '=')
                return false;
            for (size_t i = 0; i + 1 < text.size(); ++i) {
                if (!isBase64Char(text[i]))
                    return false;
            }
            uint8_t bytes[8] = {0};
            int     v[11];
            for (size_t i = 0; i < 11; ++i) {
                v[i] = b64Val(text[i]);
                if (v[i] < 0)
                    return false;
            }
            bytes[0] = static_cast<uint8_t>((v[0] << 2) | (v[1] >> 4));
            bytes[1] = static_cast<uint8_t>((v[1] << 4) | (v[2] >> 2));
            bytes[2] = static_cast<uint8_t>((v[2] << 6) | v[3]);
            bytes[3] = static_cast<uint8_t>((v[4] << 2) | (v[5] >> 4));
            bytes[4] = static_cast<uint8_t>((v[5] << 4) | (v[6] >> 2));
            bytes[5] = static_cast<uint8_t>((v[6] << 6) | v[7]);
            bytes[6] = static_cast<uint8_t>((v[8] << 2) | (v[9] >> 4));
            bytes[7] = static_cast<uint8_t>((v[9] << 4) | (v[10] >> 2));
            /* Pad bits of the last char must be zero for canonical form. */
            if (v[10] & 0x3)
                return false;
            /* Re-encode and compare. */
            static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string        re;
            uint32_t           acc  = 0;
            int                bits = 0;
            for (int i = 0; i < 8; ++i) {
                acc = (acc << 8) | bytes[i];
                bits += 8;
                while (bits >= 6) {
                    bits -= 6;
                    re += tbl[(acc >> bits) & 0x3f];
                }
            }
            /* 64 bits make 10 characters with 4 bits left over, which are the
             * high bits of the 11th; its 2 low bits are padding */
            re += tbl[(acc << (6 - bits)) & 0x3f];
            re += '=';
            return re == text;
        }

        std::string_view trimOws(std::string_view s)
        {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t'))
                ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
                --e;
            return s.substr(b, e - b);
        }

        /* Folds ASCII uppercase only. std::tolower() depends on the
         * application's locale: in a Turkish one, 'I' doesn't fold to 'i',
         * and in an 8-bit one, non-ASCII bytes can fold to ASCII letters. */
        char asciiLower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        /* Header names are ASCII and case-insensitive (RFC 9110) */
        bool headerNameIs(std::string_view name, std::string_view expected)
        {
            return std::ranges::equal(name, expected,
                                      [](char a, char b) { return asciiLower(a) == asciiLower(b); });
        }

    } // namespace

    std::optional<HeaderLine> splitHeaderLine(std::string_view line)
    {
        size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            return std::nullopt;
        const HeaderLine header{trimOws(line.substr(0, colon)), trimOws(line.substr(colon + 1))};
        if (header.name.empty())
            return std::nullopt;
        return header;
    }

    std::optional<PrepareReply> parsePrepareReply(int httpStatus, std::string_view headers)
    {
        PrepareReply out;
        out.httpStatus = httpStatus;
        size_t pos     = 0;
        while (pos < headers.size()) {
            size_t eol = headers.find("\r\n", pos);
            if (eol == std::string_view::npos)
                eol = headers.size();
            const std::string_view line = headers.substr(pos, eol - pos);
            pos                         = (eol == headers.size()) ? headers.size() : eol + 2;
            if (line.empty())
                continue;
            const auto header = splitHeaderLine(line);
            if (!header)
                return std::nullopt;
            const auto [name, value] = *header;
            if (headerNameIs(name, kProtocolHeader) && value == kProtocolValue) {
                out.protocolEcho = true;
            }
            else if (headerNameIs(name, kUnsupportedHeader) && value == kUnsupportedValue) {
                out.unsupportedMarker = true;
            }
            else if (headerNameIs(name, kReplyHeader)) {
                /* Reply keeps the legacy "200:<88hex>" shape; the peer token is
                 * the payload after the prefix. */
                constexpr std::string_view kReplyOkPrefix = "200:";
                if (!value.starts_with(kReplyOkPrefix))
                    return std::nullopt;
                const std::string_view token = value.substr(kReplyOkPrefix.size());
                if (token.size() != kTokenHexLen)
                    return std::nullopt;
                out.serverToken = token;
            }
            else if (headerNameIs(name, kSessionHeader)) {
                out.session = value;
            }
            else if (headerNameIs(name, kPsnHeader)) {
                const auto psn = parsePsn(value);
                if (!psn)
                    return std::nullopt;
                out.serverPsn = *psn;
            }
        }
        if (out.httpStatus == 200) {
            if (!out.protocolEcho)
                return std::nullopt;
            if (!isValidSessionHex(out.session))
                return std::nullopt;
            if (out.serverToken.size() != kTokenHexLen)
                return std::nullopt;
            if (out.serverPsn == 0)
                return std::nullopt;
        }
        return out;
    }

    std::optional<FinalReply> parseFinalReply(int httpStatus, std::string_view headers)
    {
        FinalReply out;
        out.httpStatus   = httpStatus;
        bool   sawCookie = false;
        size_t pos       = 0;
        while (pos < headers.size()) {
            size_t eol = headers.find("\r\n", pos);
            if (eol == std::string_view::npos)
                eol = headers.size();
            const std::string_view line = headers.substr(pos, eol - pos);
            pos                         = (eol == headers.size()) ? headers.size() : eol + 2;
            if (line.empty())
                continue;
            const auto header = splitHeaderLine(line);
            if (!header)
                return std::nullopt;
            const auto [name, value] = *header;
            if (headerNameIs(name, kProtocolHeader) && value == kProtocolValue) {
                out.protocolEcho = true;
            }
            else if (headerNameIs(name, kCookieHeader)) {
                if (value.size() != kCookieHexLen)
                    return std::nullopt;
                const auto cookie = parseNumber<uint32_t>(value, 16);
                if (!cookie)
                    return std::nullopt;
                out.cookieEcho = *cookie;
                sawCookie      = true;
            }
            else if (headerNameIs(name, kBytesHeader)) {
                /* A count that doesn't fit is malformed, not wrapped */
                const auto bytes = parseNumber<uint64_t>(value);
                if (!bytes)
                    return std::nullopt;
                out.bytes = *bytes;
            }
            else if (headerNameIs(name, kEtagHeader)) {
                out.etag = value;
            }
            else if (headerNameIs(name, kVersionHeader)) {
                out.versionId = value;
            }
            else if (headerNameIs(name, kChecksumHeader)) {
                const auto checksum = validateChecksumText(value);
                if (!checksum)
                    return std::nullopt;
                out.checksumB64 = *checksum;
            }
        }
        if ((httpStatus == 200 || httpStatus == 204) && !out.protocolEcho) {
            return std::nullopt;
        }
        if ((httpStatus == 200 || httpStatus == 204) && !sawCookie) {
            return std::nullopt;
        }
        out.cookiePresent = sawCookie;
        return out;
    }

    std::optional<std::string_view> validateChecksumText(std::string_view headerValue)
    {
        constexpr std::string_view kPrefix = "CRC64NVME ";
        if (!headerValue.starts_with(kPrefix))
            return std::nullopt;
        const std::string_view text = trimOws(headerValue.substr(kPrefix.size()));
        if (!canonicalBase64_8Bytes(text))
            return std::nullopt;
        return text;
    }

    bool isValidSessionHex(std::string_view s)
    {
        return s.size() == kSessionHexLen && std::ranges::all_of(s, isHexDigit);
    }

    std::optional<uint32_t> parsePsn(std::string_view s)
    {
        if (s.size() != kPsnHexLen)
            return std::nullopt;
        const auto v = parseNumber<uint32_t>(s, 16);
        if (!v || *v == 0 || *v > 0xffffff)
            return std::nullopt;
        return v;
    }

    std::string formatCookie(uint32_t cookie)
    {
        return formatHex(cookie, kCookieHexLen);
    }

    std::string formatPsn(uint32_t psn)
    {
        /* PSNs are 24 bits; masking also bounds the output to 6 hex digits */
        return formatHex(psn & 0xffffffU, kPsnHexLen);
    }

    std::string buildTarget(std::string_view bucket, std::string_view key, std::string_view canonicalQuery)
    {
        static const char *hex = "0123456789ABCDEF";
        std::string        out = "/";
        auto               enc = [&](std::string_view s, bool keepSlash) {
            for (char ch : s) {
                /* Percent-encoding works on raw bytes, so view each char as
                 * its 0-255 byte value (well-defined for negative chars). */
                const auto c = static_cast<unsigned char>(ch);
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '.' || c == '_' || c == '~' || (keepSlash && c == '/')) {
                    out += static_cast<char>(c);
                }
                else {
                    out += '%';
                    out += hex[c >> 4];
                    out += hex[c & 0xf];
                }
            }
        };
        enc(bucket, false);
        out += '/';
        enc(key, true);
        if (!canonicalQuery.empty()) {
            out += '?';
            out += canonicalQuery;
        }
        return out;
    }

} // namespace v2
} // namespace hipObj
