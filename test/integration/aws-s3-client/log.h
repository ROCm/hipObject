/* SPDX-License-Identifier: MIT
 *
 * Minimal leveled logging for HipObjAwsS3Client. Everything goes to
 * stderr; verbosity is controlled by HIPOBJ_LOG_LEVEL
 * (ERROR|WARN|INFO|DEBUG, default INFO) so callers decide how much
 * they want to see without recompiling. Mirrors src/common/log.h --
 * duplicated rather than shared because this target only gets
 * hipObject's public include dir, not its private src/common/.
 */

#pragma once

#include <cstdio>
#include <cstdlib>

#include <strings.h>

namespace hipobj::aws {

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

} // namespace hipobj::aws

#define HIPOBJ_AWS_LOG_IMPL(level, levelName, fmt, ...)                    \
  do {                                                                     \
    if (static_cast<int>(level) <=                                        \
        static_cast<int>(::hipobj::aws::currentLogLevel())) {             \
      std::fprintf(stderr, "[HipObjAwsS3Client][" levelName "] " fmt "\n", \
                  ##__VA_ARGS__);                                          \
    }                                                                      \
  } while (0)

#define LOG_ERROR(fmt, ...) \
  HIPOBJ_AWS_LOG_IMPL(::hipobj::aws::LogLevel::Error, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
  HIPOBJ_AWS_LOG_IMPL(::hipobj::aws::LogLevel::Warn, "WARN", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
  HIPOBJ_AWS_LOG_IMPL(::hipobj::aws::LogLevel::Info, "INFO", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) \
  HIPOBJ_AWS_LOG_IMPL(::hipobj::aws::LogLevel::Debug, "DEBUG", fmt, ##__VA_ARGS__)
