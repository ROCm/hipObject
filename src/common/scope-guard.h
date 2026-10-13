/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Runs a cleanup on every way out of a scope except success.
 *
 * Create one right after acquiring something that a later failure has
 * to give back, and call dismiss() once the success path has handed it
 * on. Every other return, and an exception unwinding through the scope,
 * runs the cleanup, so the failure paths don't each have to repeat it.
 * The standard has no equivalent: std::experimental::scope_exit is only
 * in a Technical Specification.
 *
 *     ScopeGuard rollback([&] { ring.unreserve(slot); });
 *     ...
 *     rollback.dismiss();
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

    ~ScopeGuard()
    {
        if (active_) {
            cleanup_();
        }
    }

    /* Not copyable or movable, so the cleanup can only run once */
    ScopeGuard(const ScopeGuard &)            = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ScopeGuard(ScopeGuard &&)                 = delete;
    ScopeGuard &operator=(ScopeGuard &&)      = delete;

    /* Keeps the cleanup from running */
    void dismiss() noexcept
    {
        active_ = false;
    }

private:
    F    cleanup_;
    bool active_ = true;
};

/* Lets ScopeGuard guard([&] { ... }) deduce the lambda's type */
template <typename F> ScopeGuard(F) -> ScopeGuard<F>;

} // namespace hipObj
