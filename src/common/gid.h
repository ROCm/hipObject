/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * The RDMA GID type.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hipObj {

/* Bytes in a GID */
inline constexpr size_t kGidLen = 16;

/* A RoCE or InfiniBand GID, with its bytes in the order they have in the
 * device's GID table and in an RDMA token. union ibv_gid holds the same
 * bytes, and ibv-gid.h converts between the two. This header doesn't
 * include ibv-core.h, so code built against the real libibverbs headers
 * can use it too. */
using Gid = std::array<uint8_t, kGidLen>;

/* The first 12 bytes of an IPv4-mapped IPv6 address (::ffff:a.b.c.d),
 * which is the form a RoCEv2 GID for an IPv4 address takes. The last four
 * bytes are the IPv4 address. */
inline constexpr auto kIpv4MappedPrefix = std::to_array<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff});

/* True when gid is an IPv4-mapped IPv6 address */
constexpr bool
isIpv4MappedGid(const Gid &gid) noexcept
{
    return std::ranges::equal(std::span(gid).first<kIpv4MappedPrefix.size()>(), kIpv4MappedPrefix);
}

/* The IPv4 address in an IPv4-mapped GID, most significant byte first */
constexpr std::span<const uint8_t, 4>
ipv4OfGid(const Gid &gid) noexcept
{
    return std::span(gid).last<4>();
}

} // namespace hipObj
