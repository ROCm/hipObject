/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The values of the entries in ibv-layout-list.h, from
 * <infiniband/verbs.h>, for test-ibv-layout.cpp to compare with
 * ibv-core.h. They're built by ibv-layout-real.c, which is C, so this
 * header is included by C and C++.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the values, in the order of ibv-layout-list.h, and sets
 * *count to the number of them
 */
const int64_t *hipobj_ibv_layout_real(size_t *count);

#ifdef __cplusplus
}
#endif
