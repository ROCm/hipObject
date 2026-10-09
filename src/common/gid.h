/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * The RDMA GID type.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hipObj {

/* Bytes in a GID */
inline constexpr size_t kGidLen = 16;

/* A RoCE or InfiniBand GID, with its bytes in the order they have in the
 * device's GID table and in an RDMA token. union ibv_gid holds the same
 * bytes, and ibv-gid.h converts between the two. This header doesn't
 * include ibv-core.h, so code built against the real libibverbs headers
 * can use it too. */
using Gid = std::array<uint8_t, kGidLen>;

} // namespace hipObj
