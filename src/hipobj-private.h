/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <cstdio>

#define HIPOBJ_CHECK_NULL(ptr, msg)               \
  do {                                            \
    if (!(ptr)) {                                 \
      fprintf(stderr,                             \
              "hipobj: %s is null\n", msg);       \
      return {hipObjInvalidValue, 0};             \
    }                                             \
  } while (0)

namespace hipObj {

constexpr size_t MAX_MR_SIZE =
  4ULL * 1024 * 1024 * 1024;

} // namespace hipObj
