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
 * Dismiss it after the last thing that can fail, including building the
 * result the success path returns (see dismissAfter()).
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

    /* Keeps the cleanup from running, and passes value through. The
     * argument is built before the guard is dismissed, so a return
     * statement can hand ownership on only once its result exists:
     *
     *     return guard.dismissAfter(buildReply());
     *
     * If buildReply() throws, the cleanup still runs. Returning a named
     * result after dismiss() would do the same, but costs a copy when
     * the function also returns other values (GCC's -Wnrvo).
     *
     * The guard is dismissed before value is moved out to the caller, so
     * T's move constructor must not throw: if it did, the result would be
     * lost without the cleanup running. */
    template <typename T> [[nodiscard]] T dismissAfter(T value) noexcept
    {
        static_assert(std::is_nothrow_move_constructible_v<T>,
                      "dismissAfter() dismisses the guard before moving the result out");
        active_ = false;
        return value;
    }

private:
    F    cleanup_;
    bool active_ = true;
};

/* Lets ScopeGuard guard([&] { ... }) deduce the lambda's type */
template <typename F> ScopeGuard(F) -> ScopeGuard<F>;

} // namespace hipObj
