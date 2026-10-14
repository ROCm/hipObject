/* SPDX-License-Identifier: MIT
 *
 * Minimal leveled logging for hipObject's own internals. Everything
 * goes to stderr; verbosity is controlled by HIPOBJ_LOG_LEVEL
 * (ERROR|WARN|INFO|DEBUG, default INFO) so callers decide how much
 * they want to see without recompiling.
 */

#pragma once

#include <cstdio>
#include <cstdlib>

#include <strings.h>
#include <sys/time.h>
#include <time.h>

namespace hipObj {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

inline LogLevel currentLogLevel() {
  static LogLevel level = [] {
    const char* env = std::getenv("HIPOBJ_LOG_LEVEL");
    if (!env) {
      return LogLevel::Info;
    }
    if (strcasecmp(env, "ERROR") == 0) {
      return LogLevel::Error;
    }
    if (strcasecmp(env, "WARN") == 0) {
      return LogLevel::Warn;
    }
    if (strcasecmp(env, "DEBUG") == 0) {
      return LogLevel::Debug;
    }
    return LogLevel::Info;
  }();
  return level;
}

inline void printLogTimestamp() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tmVal;
  localtime_r(&tv.tv_sec, &tmVal);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmVal);
  std::fprintf(stderr, "%s.%06ld ", buf, static_cast<long>(tv.tv_usec));
}

} // namespace hipObj

#define HIPOBJ_LOG_IMPL(level, levelName, fmt, ...)                        \
  do {                                                                     \
    if (static_cast<int>(level) <=                                        \
        static_cast<int>(::hipObj::currentLogLevel())) {                  \
      ::hipObj::printLogTimestamp();                                      \
      std::fprintf(stderr, "[hipObj][" levelName "] " fmt "\n",            \
                  ##__VA_ARGS__);                                          \
    }                                                                      \
  } while (0)

#define LOG_ERROR(fmt, ...) \
  HIPOBJ_LOG_IMPL(::hipObj::LogLevel::Error, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
  HIPOBJ_LOG_IMPL(::hipObj::LogLevel::Warn, "WARN", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
  HIPOBJ_LOG_IMPL(::hipObj::LogLevel::Info, "INFO", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) \
  HIPOBJ_LOG_IMPL(::hipObj::LogLevel::Debug, "DEBUG", fmt, ##__VA_ARGS__)
