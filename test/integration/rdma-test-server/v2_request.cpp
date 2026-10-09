/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_request.h"

#include <cctype>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <utility>

#include "hipobj-parse.h"

namespace hipObj {
namespace v2 {

    namespace {

        bool isHexDigits(const std::string &s, size_t lo, size_t hi)
        {
            if (s.size() < lo || s.size() > hi) {
                return false;
            }
            for (char c : s) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    return false;
                }
            }
            return true;
        }

        /* Parses bare hex (no 0x prefix) of at most as many digits as T
         * holds, so leading zeros can't pad a field past its width */
        template <std::unsigned_integral T> std::optional<T> parseHex(std::string_view s)
        {
            if (s.size() > sizeof(T) * 2) {
                return std::nullopt;
            }
            return parseNumber<T>(s, 16);
        }

        /* Extracts the exact Authorization header value from the raw block
         * (case-insensitive name match, value preserved byte-for-byte
         * except leading/trailing OWS). */
        std::string rawAuthorization(const std::string &raw)
        {
            const std::string needle = "authorization:";
            std::string       lower;
            lower.reserve(raw.size());
            for (char c : raw) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            size_t pos = 0;
            while (pos < lower.size()) {
                size_t lineEnd = lower.find("\r\n", pos);
                if (lineEnd == std::string::npos) {
                    lineEnd = lower.size();
                }
                size_t lineLen = lineEnd - pos;
                if (lower.compare(pos, lineLen < needle.size() ? lineLen : needle.size(), needle) == 0 &&
                    lineLen >= needle.size()) {
                    std::string v = raw.substr(pos + needle.size(), lineLen - needle.size());
                    size_t      b = v.find_first_not_of(" \t");
                    size_t      e = v.find_last_not_of(" \t");
                    if (b == std::string::npos) {
                        return std::string();
                    }
                    return v.substr(b, e - b + 1);
                }
                pos = lineEnd + 2;
            }
            return std::string();
        }

    } // namespace

    std::optional<PrepareRequest> parsePrepareRequest(const std::map<std::string, std::string> &headers,
                                                      const std::string                        &rawHeaders)
    {
        PrepareRequest out;
        auto           it = headers.find("x-amz-rdma-protocol");
        if (it == headers.end()) {
            return std::nullopt;
        }
        out.protocol = it->second;

        it = headers.find("x-amz-rdma-token");
        if (it == headers.end()) {
            return std::nullopt;
        }
        /* 88-hex or 88-hex:addr:size */
        const std::string &tok    = it->second;
        size_t             colon1 = tok.find(':');
        if (colon1 == std::string::npos) {
            if (!isHexDigits(tok, 88, 88)) {
                return std::nullopt;
            }
        }
        else {
            size_t colon2 = tok.find(':', colon1 + 1);
            if (colon2 == std::string::npos) {
                return std::nullopt;
            }
            std::string base = tok.substr(0, colon1);
            std::string addr = tok.substr(colon1 + 1, colon2 - colon1 - 1);
            std::string size = tok.substr(colon2 + 1);
            if (!isHexDigits(base, 88, 88) || addr.empty() || size.empty() || !isHexDigits(addr, 1, 16) ||
                !isHexDigits(size, 1, 16)) {
                return std::nullopt;
            }
        }
        out.token = tok;

        it = headers.find("x-amz-rdma-psn");
        if (it == headers.end()) {
            return std::nullopt;
        }
        const auto psn = parseHex<uint32_t>(it->second);
        if (!psn || *psn == 0 || *psn > 0x00ffffff) {
            return std::nullopt;
        }
        out.clientPsn = *psn;

        it = headers.find("x-amz-rdma-cookie");
        if (it == headers.end() || it->second.size() != 8) {
            return std::nullopt;
        }
        const auto cookie = parseHex<uint32_t>(it->second);
        if (!cookie) {
            return std::nullopt;
        }
        out.cookie = *cookie;

        it = headers.find("x-amz-rdma-op");
        if (it == headers.end() || (it->second != "GET" && it->second != "PUT")) {
            return std::nullopt;
        }
        out.op = it->second;

        it = headers.find("x-amz-rdma-target");
        if (it == headers.end() || it->second.empty() || it->second.front() != '/') {
            return std::nullopt;
        }
        out.target = it->second;

        it = headers.find("x-amz-rdma-size");
        if (it == headers.end()) {
            return std::nullopt;
        }
        const auto size = parseNumber<uint64_t>(it->second);
        if (!size || *size == 0) {
            return std::nullopt;
        }
        out.size = *size;

        it = headers.find("x-amz-rdma-offset");
        if (it != headers.end()) {
            const auto offset = parseNumber<uint64_t>(it->second);
            if (!offset) {
                return std::nullopt;
            }
            out.offset    = *offset;
            out.hasOffset = true;
        }

        out.authorization = rawAuthorization(rawHeaders);
        if (out.authorization.empty()) {
            return std::nullopt;
        }
        return out;
    }

    std::optional<ReadyRequest> parseReadyRequest(const std::map<std::string, std::string> &headers,
                                                  const std::string                        &rawHeaders)
    {
        ReadyRequest out;
        auto         it = headers.find("x-amz-rdma-protocol");
        if (it == headers.end()) {
            return std::nullopt;
        }
        out.protocol = it->second;

        it = headers.find("x-amz-rdma-session");
        if (it == headers.end() || !isHexDigits(it->second, 32, 32)) {
            return std::nullopt;
        }
        out.session = it->second;

        it = headers.find("x-amz-rdma-cookie");
        if (it == headers.end() || it->second.size() != 8) {
            return std::nullopt;
        }
        const auto cookie = parseHex<uint32_t>(it->second);
        if (!cookie) {
            return std::nullopt;
        }
        out.cookie = *cookie;

        /* Client MR endpoint for the data phase. Optional on a GET that
         * the server stages itself, required for PUT delivery and the
         * GET READ pull. Parsed as bare hex without a 0x prefix.
         * Present-but-empty fails too: a field that exists must carry a
         * valid value. */
        it = headers.find("x-amz-rdma-mr-addr");
        if (it != headers.end()) {
            const auto addr = parseHex<uint64_t>(it->second);
            if (!addr) {
                return std::nullopt;
            }
            out.mrAddr = *addr;
        }
        it = headers.find("x-amz-rdma-mr-rkey");
        if (it != headers.end()) {
            const auto rkey = parseHex<uint32_t>(it->second);
            if (!rkey) {
                return std::nullopt;
            }
            out.mrRkey = *rkey;
        }
        it = headers.find("x-amz-rdma-qpn");
        if (it != headers.end()) {
            const auto qpn = parseHex<uint32_t>(it->second);
            if (!qpn) {
                return std::nullopt;
            }
            out.qpn = *qpn;
        }

        out.authorization = rawAuthorization(rawHeaders);
        if (out.authorization.empty()) {
            return std::nullopt;
        }
        return out;
    }

    std::optional<CancelRequest> parseCancelRequest(const std::map<std::string, std::string> &headers,
                                                    const std::string                        &rawHeaders)
    {
        CancelRequest out;
        auto          it = headers.find("x-amz-rdma-protocol");
        if (it == headers.end()) {
            return std::nullopt;
        }
        out.protocol = it->second;

        it = headers.find("x-amz-rdma-session");
        if (it == headers.end() || !isHexDigits(it->second, 32, 32)) {
            return std::nullopt;
        }
        out.session = it->second;

        out.authorization = rawAuthorization(rawHeaders);
        if (out.authorization.empty()) {
            return std::nullopt;
        }
        return out;
    }

} // namespace v2
} // namespace hipObj
