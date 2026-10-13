/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Parsing of the x-amz-rdma-token header the test server receives */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "hipobj-parse.h"
#include "token.h"

namespace hipobj::test {

/* A parsed x-amz-rdma-token value */
struct TokenHeader {
    hipObj::RdmaToken token;      /* With the suffix's overrides applied */
    size_t            length = 0; /* The suffix's length, or 0 if there's no suffix or it gave 0 */
};

/* Parses an x-amz-rdma-token value: an encoded token, optionally followed
 * by ":<addr>:<len>" in hex. A nonzero address or length overrides the
 * token's. Returns std::nullopt if the token doesn't decode, or if the
 * suffix doesn't have exactly two fields that each parse in full. */
inline std::optional<TokenHeader>
parseTokenHeader(std::string_view header)
{
    const size_t c1      = header.find(':');
    const auto   decoded = hipObj::decodeRdmaTokenHex(header.substr(0, c1));
    if (!decoded) {
        return std::nullopt;
    }
    TokenHeader out{*decoded, 0};
    if (c1 == std::string_view::npos) {
        return out;
    }
    const size_t c2 = header.find(':', c1 + 1);
    if (c2 == std::string_view::npos) {
        return std::nullopt;
    }
    /* A third ':' lands in the length text, which then fails to parse */
    const std::optional<uint64_t> addr =
        hipObj::parseNumber<uint64_t>(header.substr(c1 + 1, c2 - c1 - 1), 16);
    const std::optional<size_t> len = hipObj::parseNumber<size_t>(header.substr(c2 + 1), 16);
    if (!addr || !len) {
        return std::nullopt;
    }
    if (*addr != 0) {
        out.token.remoteAddr = *addr;
    }
    if (*len != 0) {
        out.token.length = *len;
    }
    out.length = *len;
    return out;
}

} // namespace hipobj::test
