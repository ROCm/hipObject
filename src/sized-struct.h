/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Reading the public structs that start with structSize (see "Struct
 * Sizes" in hipobj.h)
 *
 * An application sets structSize to the size of the struct in the header
 * it was compiled with. Fields are only ever added to the end of a
 * struct, so a smaller structSize is an older version of it, and a larger
 * one a newer version. A field that's added has to do what the version
 * before it did when it's zero, since that's what an older application
 * gets. */

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

#include "hipobj-span.h"
#include "hipobj.h"

namespace hipObj {

/* The largest structSize accepted. It limits how far past the end of a
 * struct readSizedStruct() reads, so that a structSize that was never set
 * can't send it far into memory. */
constexpr size_t kMaxStructSize = 4096;

/* The sizes of hipObjConfig_t and hipObjOps_t in hipObject 0.1.0, the
 * first version with structSize: the end of the last field it had. They
 * don't change when fields are added. */
constexpr size_t kConfigMinSize = offsetof(hipObjConfig_t, nicHint) + sizeof(hipObjConfig_t::nicHint);
constexpr size_t kOpsMinSize    = offsetof(hipObjOps_t, recvReply) + sizeof(hipObjOps_t::recvReply);

/* A copy of the T that the application passed at in, which starts with
 * structSize. It has T's fields from in, as far as structSize reaches,
 * and zero in any of T's fields after that. nullopt if in is null; if
 * structSize is below MinSize, the size of T in the first version with
 * structSize, or above kMaxStructSize; or if any of the bytes after T's
 * fields, from a newer version of T, isn't zero.
 *
 * in is a pointer to void, and every byte is read as a byte, because a
 * struct from an older application is shorter than T, so nothing may read
 * it as a T. */
template <typename T, size_t MinSize>
    requires std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T> &&
             std::is_standard_layout_v<T>
[[nodiscard]] std::optional<T>
readSizedStruct(const void *in) noexcept
{
    static_assert(offsetof(T, structSize) == 0 && std::is_same_v<decltype(T::structSize), size_t>,
                  "structSize must be the first field");
    static_assert(MinSize >= sizeof(size_t) && MinSize <= sizeof(T), "MinSize must be within T");

    if (in == nullptr) {
        return std::nullopt;
    }
    const auto *start = static_cast<const std::byte *>(in);

    size_t structSize = 0;
    std::ranges::copy(spanFromPointer(start, sizeof(structSize)),
                      std::as_writable_bytes(std::span<size_t, 1>{&structSize, 1}).begin());
    if (structSize < MinSize || structSize > kMaxStructSize) {
        return std::nullopt;
    }

    const std::span<const std::byte> given = spanFromPointer(start, structSize);
    const size_t                     known = std::min(structSize, sizeof(T));
    if (!std::ranges::all_of(given.subspan(known), [](std::byte b) noexcept { return b == std::byte{0}; })) {
        return std::nullopt;
    }
    T copy{};
    std::ranges::copy(given.first(known), std::as_writable_bytes(std::span<T, 1>{&copy, 1}).begin());
    return copy;
}

} // namespace hipObj
