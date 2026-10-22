/* SPDX-License-Identifier: MIT
 *
 * Minimal leveled logging for HipObjAwsS3Client. Everything goes to
 * stderr; verbosity is controlled by HIPOBJ_LOG_LEVEL
 * (ERROR|WARN|INFO|DEBUG, default INFO) so callers decide how much
 * they want to see without recompiling. It reads the same variable as
 * the library's src/common/log.h, which it can't include: this target
 * gets hipObject's public include dir and shared/, not its private
 * src/common/.
 *
 * Messages are std::format() strings. Their arguments are evaluated even
 * when the level is off, so check logEnabled() first where they cost
 * something.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

#include "hipobj-print.h"
#include "hipobj-span.h"

namespace hipobj::aws {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

/* True when value is name, an upper-case level name, in any case */
inline bool
isLogLevelName(std::string_view value, std::string_view name) noexcept
{
    return std::ranges::equal(value, name, [](char v, char n) noexcept {
        return std::toupper(static_cast<unsigned char>(v)) == static_cast<unsigned char>(n);
    });
}

/* The level HIPOBJ_LOG_LEVEL names, read the first time a message is
 * logged. No value, or any other, is INFO. */
inline LogLevel
currentLogLevel() noexcept
{
    static const LogLevel level = []() noexcept {
        const char *env = std::getenv("HIPOBJ_LOG_LEVEL");
        if (env == nullptr) {
            return LogLevel::Info;
        }
        /* One more than the longest name, so a longer value matches none */
        constexpr size_t       kMaxNameLen = 6;
        const std::string_view value       = hipObj::cStringView(env, kMaxNameLen);
        if (isLogLevelName(value, "ERROR")) {
            return LogLevel::Error;
        }
        if (isLogLevelName(value, "WARN")) {
            return LogLevel::Warn;
        }
        if (isLogLevelName(value, "DEBUG")) {
            return LogLevel::Debug;
        }
        return LogLevel::Info;
    }();
    return level;
}

inline bool
logEnabled(LogLevel level) noexcept
{
    return level <= currentLogLevel();
}

/* Writes "[HipObjAwsS3Client][<levelName>] <message>" and a newline to
 * stderr. A message that can't be formatted is dropped. */
template <typename... Args>
void
logAt(LogLevel level, std::string_view levelName, std::format_string<Args...> fmt, Args &&...args) noexcept
{
    if (!logEnabled(level)) {
        return;
    }
    try {
        hipObj::printTo(stderr, "[HipObjAwsS3Client][{}] {}\n", levelName,
                        std::format(fmt, std::forward<Args>(args)...));
    }
    catch (...) {
        /* There's nowhere left to report the failure */
    }
}

template <typename... Args>
void
logError(std::format_string<Args...> fmt, Args &&...args) noexcept
{
    logAt(LogLevel::Error, "ERROR", fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void
logWarn(std::format_string<Args...> fmt, Args &&...args) noexcept
{
    logAt(LogLevel::Warn, "WARN", fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void
logInfo(std::format_string<Args...> fmt, Args &&...args) noexcept
{
    logAt(LogLevel::Info, "INFO", fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void
logDebug(std::format_string<Args...> fmt, Args &&...args) noexcept
{
    logAt(LogLevel::Debug, "DEBUG", fmt, std::forward<Args>(args)...);
}

} // namespace hipobj::aws
