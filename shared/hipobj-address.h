/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Conversions between pointers and the integer addresses they hold
 *
 * RDMA describes memory by its address: memory registrations are keyed
 * by it, and a remote peer reads and writes at it. Converting a pointer
 * to an integer, or back, takes a reinterpret_cast, which clang-tidy's
 * cppcoreguidelines-pro-type-reinterpret-cast reports wherever it
 * appears, since most reinterpret_casts reinterpret memory as another
 * type. These conversions don't, so the functions here make them, and
 * are the only places that suppress the check. */

#pragma once

#include <cstdint>
#include <type_traits>

namespace hipObj {

/* The address that ptr holds */
inline uintptr_t
pointerToAddress(const void *ptr) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<uintptr_t>(ptr);
}

/* A pointer of type Ptr that holds address. Tests use it to make fake
 * pointers, which are only compared and passed along, never
 * dereferenced. */
template <typename Ptr>
    requires std::is_pointer_v<Ptr> && (!std::is_function_v<std::remove_pointer_t<Ptr>>)
Ptr addressToPointer(uintptr_t address) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    return reinterpret_cast<Ptr>(address);
}

} // namespace hipObj
