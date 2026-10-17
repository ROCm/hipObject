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
 * use the view from then on. writeChars() goes the other way, handing a
 * view to fwrite() as a pointer and a length.
 *
 * Print with hipObj::printTo() (hipobj-print.h), not printf() and its
 * relatives: clang-tidy reports C-style variadic calls. */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
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

/* Writes chars to stream with one fwrite(). True if they were all
 * written. */
inline bool
writeChars(std::FILE *stream, std::span<const char> chars) noexcept
{
    return std::fwrite(chars.data(), 1, chars.size(), stream) == chars.size();
}

HIPOBJ_WARN_UNSAFE_BUFFER_ON

/* Views the NUL-terminated string in chars, without its NUL, or all of
 * chars if it has no NUL. Pass it a char array, such as ibv_device's
 * name, without decaying the array to a pointer. */
inline std::string_view
cStringView(std::span<const char> chars) noexcept
{
    return cStringView(chars.data(), chars.size());
}

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

/* Returns a T with every byte zero, padding included, as zeroObject()
 * leaves one. Declare a C struct with it, as in
 * auto attr = zeroedObject<ibv_qp_attr>(), rather than declaring it
 * uninitialized and zeroing it with zeroObject().
 *
 * Neither T() nor T{} is enough on its own. The standard says T()
 * zero-initializes a C struct, padding included, but GCC 13 sets only the
 * members, and T{} doesn't have to zero the padding at all. So this
 * zeroes every byte with zeroObject().
 *
 * The object is returned by name, and every supported compiler constructs
 * it in place of the object being declared with the result (the named
 * return value optimization), so the padding isn't lost to a copy. The
 * standard doesn't require that, but -Wnrvo (GCC 14 and later) reports a
 * function where it doesn't happen, and the tests check the padding with
 * every compiler. Assigning the result to an existing object is a copy,
 * though, and a copy needn't keep the padding, so use zeroObject() for
 * that. */
template <typename T>
    requires std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T>
[[nodiscard]] T
zeroedObject() noexcept
{
    T obj{};
    zeroObject(obj);
    return obj;
}

} // namespace hipObj
