/* SPDX-License-Identifier: MIT
 *
 * Minimal leveled logging for hipObject's own internals. Everything
 * goes to stderr; verbosity is controlled by HIPOBJ_LOG_LEVEL
 * (ERROR|WARN|INFO|DEBUG, default INFO) so callers decide how much
 * they want to see without recompiling.
 *
 * Messages are std::format() strings: logError("bind to {} failed: {}",
 * ip, std::strerror(errno)).
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "hipobj-print.h"
#include "hipobj-span.h"

namespace hipObj {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

/* True when value is name, an upper-case level name, in any case */
inline bool
isLogLevelName(std::string_view value, std::string_view name) noexcept
{
    return std::ranges::equal(value, name, [](char v, char n) noexcept {
        return std::toupper(static_cast<unsigned char>(v)) == static_cast<unsigned char>(n);
    });
}

/* The level a HIPOBJ_LOG_LEVEL value names: ERROR, WARN, INFO or DEBUG,
 * in any case. No value, or any other, is INFO. */
inline LogLevel
logLevelFromEnv(const char *env) noexcept
{
    if (env == nullptr) {
        return LogLevel::Info;
    }
    /* One more than the longest name, so a longer value matches none */
    constexpr size_t       kMaxNameLen = 6;
    const std::string_view value       = cStringView(env, kMaxNameLen);
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
}

/* The level HIPOBJ_LOG_LEVEL names, read the first time a message is
 * logged */
inline LogLevel
currentLogLevel() noexcept
{
    static const LogLevel level = logLevelFromEnv(std::getenv("HIPOBJ_LOG_LEVEL"));
    return level;
}

/* True when messages at level are logged. logError() and the rest
 * evaluate their arguments even when they aren't, so on the data path a
 * message whose arguments do any work, such as taking a lock, goes inside
 * a check of this. */
inline bool
logEnabled(LogLevel level) noexcept
{
    return level <= currentLogLevel();
}

/* Writes "HH:MM:SS.uuuuuu [hipObj][<levelName>] <message>" and a newline
 * to stderr, with printTo(). Like printTo(), it never throws: a message
 * that can't be formatted is dropped. */
template <typename... Args>
void
logAt(LogLevel level, std::string_view levelName, std::format_string<Args...> fmt, Args &&...args) noexcept
{
    if (!logEnabled(level)) {
        return;
    }
    try {
        const std::string message = std::format(fmt, std::forward<Args>(args)...);
        const auto        now     = std::chrono::system_clock::now();
        const std::time_t secs    = std::chrono::system_clock::to_time_t(now);
        const auto        usecs =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1'000'000;
        std::tm local{};
        (void)localtime_r(&secs, &local);
        printTo(stderr, "{:02}:{:02}:{:02}.{:06} [hipObj][{}] {}\n", local.tm_hour, local.tm_min,
                local.tm_sec, usecs, levelName, message);
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

} // namespace hipObj
