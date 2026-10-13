/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Formatting of integers as text */

#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <memory>
#include <string>

namespace hipObj {

/* Formats value as lowercase hex, zero-padded on the left to at least width
 * digits, like printf()'s "%0*x". A value with more digits than width isn't
 * truncated, so a caller that needs exactly width digits masks the value
 * first. bool counts as an unsigned integral type, but std::to_chars()
 * rejects it, so it's excluded here to fail at the call rather than in the
 * body. */
template <std::unsigned_integral T>
    requires(!std::same_as<T, bool>)
std::string
formatHex(T value, size_t width = 0)
{
    /* Two hex digits per byte holds every value of T, so std::to_chars
     * can't run out of room and its error code doesn't need checking */
    std::array<char, sizeof(T) * 2> digits{};
    const auto  result = std::to_chars(digits.data(), std::to_address(digits.end()), value, 16);
    const auto  len    = static_cast<size_t>(result.ptr - digits.data());
    std::string out(width > len ? width - len : 0, '0');
    out.append(digits.data(), len);
    return out;
}

} // namespace hipObj
