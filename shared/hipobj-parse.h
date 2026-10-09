/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Strict parsing of integers from text */

#pragma once

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>
#include <system_error>

namespace hipObj {

/* Parses all of text as an integer in the given base. Unlike strtol() and
 * std::stoll(), it doesn't skip whitespace or accept a '+' sign or a "0x"
 * prefix, and an unsigned T doesn't accept a '-' sign. It fails on an empty
 * string, trailing characters, or a value that doesn't fit in T, rather than
 * stopping early or clamping. Leading zeros are accepted, so a caller that
 * needs a fixed number of digits checks the length itself. base must be 2
 * to 36, as std::from_chars requires; any other base fails. */
template <std::integral T>
std::optional<T>
parseNumber(std::string_view text, int base = 10)
{
    if (text.empty() || base < 2 || base > 36) {
        return std::nullopt;
    }
    T           value{};
    const char *end      = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

} // namespace hipObj
