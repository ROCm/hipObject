/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Type-safe formatted output, in place of printf() and fprintf() */

#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <utility>

#include "hipobj-span.h"

namespace hipObj {

/* Formats args with std::format() and writes the text to stream, as C++23's
 * std::print() does. The format string is checked against the arguments'
 * types when the call is compiled, so a mismatch is a compile error rather
 * than undefined behavior at run time, and "{}" prints any formattable
 * type, so there's no length modifier to get wrong for size_t or int64_t.
 *
 * The text is written with one fwrite(), which POSIX makes atomic with
 * respect to other stdio calls on the stream, so lines from different
 * threads don't interleave. Unlike std::print(), it never throws: text that
 * can't be formatted (out of memory) or written is dropped, so it's safe to
 * call from a destructor or an error path. */
template <typename... Args>
void
printTo(std::FILE *stream, std::format_string<Args...> fmt, Args &&...args) noexcept
{
    try {
        const std::string text = std::format(fmt, std::forward<Args>(args)...);
        (void)writeChars(stream, text);
    }
    catch (...) {
        /* There's nowhere left to report the failure */
    }
}

/* printTo() for a format string with no arguments. Overload resolution
 * prefers it to the template, so the template is never instantiated with
 * an empty args, which CodeQL reports as an unused variable. */
inline void
printTo(std::FILE *stream, std::format_string<> fmt) noexcept
{
    try {
        const std::string text = std::format(fmt);
        (void)writeChars(stream, text);
    }
    catch (...) {
        /* There's nowhere left to report the failure */
    }
}

} // namespace hipObj
