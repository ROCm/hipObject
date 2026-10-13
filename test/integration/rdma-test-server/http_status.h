/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Status-line parsing for the test clients */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "hipobj-parse.h"

namespace hipobj::test {

/* Returns the status code from the status line that starts an HTTP
 * response ("HTTP/1.1 200 OK"), or std::nullopt if the line is malformed.
 * RFC 9112 defines the line as "HTTP/" DIGIT "." DIGIT, a space, the
 * three-digit code, a space, and a reason phrase that can be empty. The
 * code also has to be in the range 100 to 599 that RFC 9110 defines. */
inline std::optional<int>
parseStatusCode(std::string_view response)
{
    constexpr std::string_view kName   = "HTTP/";
    constexpr size_t           kCodeAt = 9; /* After "HTTP/1.1 " */
    constexpr size_t           kCodeSz = 3;

    const std::string_view line    = response.substr(0, response.find("\r\n"));
    const auto             isDigit = [](char c) { return c >= '0' && c <= '9'; };
    if (line.size() < kCodeAt + kCodeSz + 1 || !line.starts_with(kName) || !isDigit(line[5]) ||
        line[6] != '.' || !isDigit(line[7]) || line[8] != ' ' || line[kCodeAt + kCodeSz] != ' ') {
        return std::nullopt;
    }
    /* Parsed as unsigned, so a sign can't stand in for a digit */
    const std::optional<uint16_t> code = hipObj::parseNumber<uint16_t>(line.substr(kCodeAt, kCodeSz));
    if (!code || *code < 100 || *code > 599) {
        return std::nullopt;
    }
    return *code;
}

} // namespace hipobj::test
