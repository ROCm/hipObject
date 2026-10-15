/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>

#include "hipobj.h"

namespace hipObj {

constexpr size_t MAX_MR_SIZE = 4ULL * 1024 * 1024 * 1024;

/* The hipObjError_t a public function returns. hipError is the HIP error
 * that caused opError, if there is one. */
[[nodiscard]] constexpr hipObjError_t
makeError(hipObjOpError_t opError, int hipError = 0) noexcept
{
    return {.opError = opError, .hipError = hipError};
}

} // namespace hipObj
