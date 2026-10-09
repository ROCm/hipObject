/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Implementation of the hipobj-rc-v2 wire helpers (see v2-wire.h). */

#include "v2-wire.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <optional>

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
        bool canonicalBase64_8Bytes(const std::string &text)
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

        std::string trimOws(const std::string &s)
        {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t'))
                ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
                --e;
            return s.substr(b, e - b);
        }

    } // namespace

    bool splitHeaderLine(const std::string &line, std::string &name, std::string &value)
    {
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            return false;
        name  = trimOws(line.substr(0, colon));
        value = trimOws(line.substr(colon + 1));
        if (name.empty())
            return false;
        return true;
    }

    bool parsePrepareReply(int httpStatus, const std::string &headers, PrepareReply &out)
    {
        out            = PrepareReply();
        out.httpStatus = httpStatus;
        size_t pos     = 0;
        while (pos < headers.size()) {
            size_t eol = headers.find("\r\n", pos);
            if (eol == std::string::npos)
                eol = headers.size();
            std::string line = headers.substr(pos, eol - pos);
            pos              = (eol == headers.size()) ? headers.size() : eol + 2;
            if (line.empty())
                continue;
            std::string name, value;
            if (!splitHeaderLine(line, name, value))
                return false;
            for (auto &c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (name == "x-amz-rdma-protocol" && value == kProtocolValue) {
                out.protocolEcho = true;
            }
            else if (name == "x-amz-rdma-protocol-status" && value == kUnsupportedValue) {
                out.unsupportedMarker = true;
            }
            else if (name == "x-amz-rdma-reply") {
                /* Reply keeps the legacy "200:<88hex>" shape; the peer token is
                 * the payload after the prefix. */
                size_t c = value.find(':');
                if (c == std::string::npos)
                    return false;
                if (value.compare(0, c, "200") != 0)
                    return false;
                out.serverToken = value.substr(c + 1);
                if (out.serverToken.size() != kTokenHexLen)
                    return false;
            }
            else if (name == "x-amz-rdma-session") {
                out.session = value;
            }
            else if (name == "x-amz-rdma-psn") {
                if (!parsePsn(value, out.serverPsn))
                    return false;
            }
        }
        if (out.httpStatus == 200) {
            if (!out.protocolEcho)
                return false;
            if (!isValidSessionHex(out.session))
                return false;
            if (out.serverToken.size() != kTokenHexLen)
                return false;
            if (out.serverPsn == 0)
                return false;
        }
        return true;
    }

    bool parseFinalReply(int httpStatus, const std::string &headers, FinalReply &out)
    {
        out              = FinalReply();
        out.httpStatus   = httpStatus;
        bool   sawCookie = false;
        size_t pos       = 0;
        while (pos < headers.size()) {
            size_t eol = headers.find("\r\n", pos);
            if (eol == std::string::npos)
                eol = headers.size();
            std::string line = headers.substr(pos, eol - pos);
            pos              = (eol == headers.size()) ? headers.size() : eol + 2;
            if (line.empty())
                continue;
            std::string name, value;
            if (!splitHeaderLine(line, name, value))
                return false;
            for (auto &c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (name == "x-amz-rdma-protocol" && value == kProtocolValue) {
                out.protocolEcho = true;
            }
            else if (name == "x-amz-rdma-cookie") {
                if (value.size() != kCookieHexLen)
                    return false;
                const auto cookie = parseNumber<uint32_t>(value, 16);
                if (!cookie)
                    return false;
                out.cookieEcho = *cookie;
                sawCookie      = true;
            }
            else if (name == "x-amz-rdma-bytes-transferred") {
                /* A count that doesn't fit is malformed, not wrapped */
                const auto bytes = parseNumber<uint64_t>(value);
                if (!bytes)
                    return false;
                out.bytes = *bytes;
            }
            else if (name == "x-amz-rdma-etag") {
                out.etag = value;
            }
            else if (name == "x-amz-rdma-version-id") {
                out.versionId = value;
            }
            else if (name == "x-amz-rdma-checksum") {
                if (!validateChecksumText(value, out.checksumB64))
                    return false;
            }
        }
        if ((httpStatus == 200 || httpStatus == 204) && !out.protocolEcho) {
            return false;
        }
        if ((httpStatus == 200 || httpStatus == 204) && !sawCookie) {
            return false;
        }
        out.cookiePresent = sawCookie;
        return true;
    }

    bool validateChecksumText(const std::string &headerValue, std::string &out)
    {
        static const char kPrefix[] = "CRC64NVME ";
        size_t            plen      = sizeof(kPrefix) - 1;
        if (headerValue.compare(0, plen, kPrefix) != 0)
            return false;
        std::string text = trimOws(headerValue.substr(plen));
        if (!canonicalBase64_8Bytes(text))
            return false;
        out = text;
        return true;
    }

    bool isValidSessionHex(const std::string &s)
    {
        if (s.size() != kSessionHexLen)
            return false;
        for (char c : s) {
            if (!isHexDigit(c))
                return false;
        }
        return true;
    }

    bool parsePsn(const std::string &s, uint32_t &psn)
    {
        if (s.size() != kPsnHexLen)
            return false;
        const auto v = parseNumber<uint32_t>(s, 16);
        if (!v || *v == 0 || *v > 0xffffff)
            return false;
        psn = *v;
        return true;
    }

    std::string formatCookie(uint32_t cookie)
    {
        char buf[kCookieHexLen + 1];
        std::snprintf(buf, sizeof(buf), "%08" PRIx32, cookie);
        return std::string(buf);
    }

    std::string formatPsn(uint32_t psn)
    {
        char buf[kPsnHexLen + 1];
        /* PSNs are 24 bits; masking also bounds the output to 6 hex digits */
        std::snprintf(buf, sizeof(buf), "%06" PRIx32, psn & 0xffffffU);
        return std::string(buf);
    }

    std::string buildTarget(const std::string &bucket, const std::string &key,
                            const std::string &canonicalQuery)
    {
        static const char *hex = "0123456789ABCDEF";
        std::string        out = "/";
        auto               enc = [&](const std::string &s, bool keepSlash) {
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
