/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Conversions between Gid and union ibv_gid.
 */

#pragma once

#include <bit>

#include "gid.h"
#include "ibv-core.h"

namespace hipObj {

/* std::bit_cast copies the bytes, and doesn't compile if the two types
 * differ in size */
inline Gid
toGid(const ibv_gid &gid)
{
    return std::bit_cast<Gid>(gid);
}

inline ibv_gid
toIbvGid(const Gid &gid)
{
    return std::bit_cast<ibv_gid>(gid);
}

} // namespace hipObj
