/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Compiled as C11, and as C++11 by header-check.cpp, to check that
 * hipobj.h is valid in both languages (see CMakeLists.txt). hipobj.h is
 * included first to check that it's self-contained.
 */

#include <hipobj.h>

/* A macro is only checked where it's expanded, so this uses every
 * macro hipobj.h defines
 */
int hipobj_header_check_macros[] = {
    HIPOBJ_VERSION_MAJOR,
    HIPOBJ_VERSION_MINOR,
    HIPOBJ_VERSION_PATCH,
    HIPOBJ_BASE_ERR,
    HIPOBJ_RDMA_OP_PUT,
    HIPOBJ_RDMA_OP_GET,
    HIPOBJ_RDMA_REPLY_NOT_IMPLEMENTED,
    HIPOBJ_SYNC_TO_HOST,
    HIPOBJ_SYNC_TO_DEVICE,
};
