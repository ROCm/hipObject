/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Runs a cleanup on every way out of a scope.
 *
 * Create one right after the point from which every return, and an
 * exception unwinding through the scope, has to run the cleanup, so the
 * return paths don't each have to repeat it. The standard has no
 * equivalent: std::experimental::scope_exit is only in a Technical
 * Specification.
 *
 *     const ScopeGuard resetQp([&] { conn.resetQp(); });
 *
 * The cleanup runs in the destructor, which is noexcept, so a cleanup
 * that throws terminates the program. */

#pragma once

#include <type_traits>
#include <utility>

namespace hipObj {

template <typename F> class [[nodiscard]] ScopeGuard {
public:
    explicit ScopeGuard(F cleanup) noexcept(std::is_nothrow_move_constructible_v<F>)
        : cleanup_(std::move(cleanup))
    {
    }

    /* Explicitly noexcept: otherwise a lambda capturing something whose
     * destructor can throw would make this one potentially throwing too */
    ~ScopeGuard() noexcept
    {
        cleanup_();
    }

    /* Not copyable or movable, so the cleanup can only run once */
    ScopeGuard(const ScopeGuard &)            = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ScopeGuard(ScopeGuard &&)                 = delete;
    ScopeGuard &operator=(ScopeGuard &&)      = delete;

private:
    F cleanup_;
};

/* Lets ScopeGuard guard([&] { ... }) deduce the lambda's type */
template <typename F> ScopeGuard(F) -> ScopeGuard<F>;

} // namespace hipObj
