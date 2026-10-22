/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Socket addresses for the sockets API
 *
 * bind(), connect(), getsockname(), and the rest take every kind of
 * socket address as a struct sockaddr *, so an IPv4 or IPv6 address has
 * to be cast to one. That takes a reinterpret_cast, which clang-tidy's
 * cppcoreguidelines-pro-type-reinterpret-cast reports, so the functions
 * here make the cast, and are the only place that suppresses the check.
 * It's a separate header from hipobj-address.h because only the tests and
 * the v3 RC-CM listener use sockets, and these headers are POSIX-only. */

#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

namespace hipObj {

/* addr, as the sockets API takes it */
inline sockaddr *
asSockaddr(sockaddr_in &addr) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr *>(&addr);
}

/* addr, as the sockets API takes it */
inline sockaddr *
asSockaddr(sockaddr_in6 &addr) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr *>(&addr);
}

} // namespace hipObj
