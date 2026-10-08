/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Move assignment for classes whose members must be released in order.
 *
 * Members are destroyed in reverse declaration order, so a class that
 * declares what it depends on first (a context before its PD, a buffer
 * before the MR that covers it) tears down correctly. A defaulted move
 * assignment replaces the members in declaration order instead, which
 * releases the target's dependencies before the objects that use them.
 *
 * replaceByMove() destroys the target, releasing its members in the
 * destructor's order, then move-constructs the source in its place. The
 * class must be final, so the target is never a base-class subobject
 * and the caller's *this refers to the new object, and its move
 * constructor must not throw, so the target is never left destroyed. */

#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace hipObj {

template <typename T>
void
replaceByMove(T &target, T &&source) noexcept
{
    static_assert(std::is_final_v<T>, "a derived object would only be partly replaced");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "a throwing move would leave the target destroyed");
    if (&target != &source) {
        std::destroy_at(&target);
        std::construct_at(&target, std::move(source));
    }
}

} // namespace hipObj
