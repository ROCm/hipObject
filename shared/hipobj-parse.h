/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Strict parsing of integers from text */

#pragma once

#include <charconv>
#include <concepts>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace hipObj {

/* The bases parseNumber() accepts, which are the ones std::from_chars
 * accepts, and its default */
inline constexpr int kMinNumberBase     = 2;
inline constexpr int kMaxNumberBase     = 36;
inline constexpr int kDefaultNumberBase = 10;

/* Parses all of text as an integer in the given base. Unlike strtol() and
 * std::stoll(), it doesn't skip whitespace or accept a '+' sign or a "0x"
 * prefix, and an unsigned T doesn't accept a '-' sign. It fails on an empty
 * string, trailing characters, or a value that doesn't fit in T, rather than
 * stopping early or clamping. Leading zeros are accepted, so a caller that
 * needs a fixed number of digits checks the length itself. Any base outside
 * [kMinNumberBase, kMaxNumberBase] fails. */
template <std::integral T>
std::optional<T>
parseNumber(std::string_view text, int base = kDefaultNumberBase)
{
    if (text.empty() || base < kMinNumberBase || base > kMaxNumberBase) {
        return std::nullopt;
    }
    T           value{};
    const char *end      = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

} // namespace hipObj
