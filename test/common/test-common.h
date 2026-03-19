/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <gtest/gtest.h>

#include "hipobj.h"

#define EXPECT_HIPOBJ_SUCCESS(expr)                                            \
  do {                                                                         \
    hipObjError_t e = (expr);                                                  \
    EXPECT_EQ(e.opError, hipObjSuccess);                                       \
  } while (0)
