/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Bounded views of memory that arrives as a raw pointer
 *
 * Clang's -Wunsafe-buffer-usage flags pointer arithmetic, indexing
 * through a raw pointer, the C library's unbounded memory and string
 * functions, and building a std::span or std::string_view from a pointer
 * and a separate length, since nothing ties the length to the memory.
 * Code should work with containers and views instead, which (with
 * _GLIBCXX_ASSERTIONS) check their bounds.
 *
 * Memory that comes through a C interface (the public API, libibverbs,
 * main()'s argv) only comes as a pointer and a length, though. The
 * functions here turn it into a view, and are the only places that
 * suppress the warning. The caller vouches that the pointer and length
 * describe valid memory, so convert once, where the pointer enters, and
 * use the view from then on.
 *
 * The warning also flags a printf() "%s" argument unless it's a string
 * literal or std::string::c_str(), since it can't tell whether other
 * pointers point to a NUL-terminated string. Copy such a string into a
 * std::string to print it (this is only worth it on error paths, where
 * most printing is). */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include "hipobj-warnings.h"

namespace hipObj {

HIPOBJ_WARN_UNSAFE_BUFFER_OFF

/* Views count objects starting at ptr */
template <typename T>
constexpr std::span<T>
spanFromPointer(T *ptr, size_t count) noexcept
{
    return {ptr, count};
}

/* Views the NUL-terminated string at str, without its NUL, scanning at
 * most maxLen bytes. A string with no NUL in its first maxLen bytes is
 * cut to maxLen bytes, so to reject a string longer than some limit,
 * pass the limit plus one and compare the length with the limit. */
inline std::string_view
cStringView(const char *str, size_t maxLen) noexcept
{
    return {str, strnlen(str, maxLen)};
}

/* Views a span of characters as a string. A span's size always matches
 * its memory, but newer versions of Clang flag this anyway. */
constexpr std::string_view
asStringView(std::span<const char> chars) noexcept
{
    return {chars.data(), chars.size()};
}

HIPOBJ_WARN_UNSAFE_BUFFER_ON

/* Views main()'s arguments */
inline std::span<char *>
argSpan(int argc, char **argv) noexcept
{
    return spanFromPointer(argv, argc > 0 ? static_cast<size_t>(argc) : 0);
}

/* Sets every byte of obj to zero, padding included. Use it on a struct
 * that's handed to libibverbs or reaches the wire, where T x{} isn't
 * enough, since it doesn't have to zero the padding. */
template <typename T>
    requires std::is_trivially_copyable_v<T>
void
zeroObject(T &obj) noexcept
{
    std::ranges::fill(std::as_writable_bytes(std::span<T, 1>{std::addressof(obj), 1}), std::byte{0});
}

} // namespace hipObj
