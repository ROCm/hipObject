/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Waiting for something that's checked by polling, such as a work
 * completion or a HIP event.
 *
 * A fixed sleep between polls notices a result late: a completion that
 * arrives in microseconds is seen a millisecond or more later. waitFor()
 * polls back to back at first, then yields the CPU between polls, and
 * only then sleeps, for longer the longer it has waited. A fast result is
 * seen at once, and a slow one doesn't keep a core busy. */

#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>

namespace hipObj {

/* What waitFor() does after a poll that found nothing */
struct PollPause {
    enum class Kind : uint8_t {
        Spin,  /* Poll again at once */
        Yield, /* Let other threads run, then poll again */
        Sleep, /* Sleep for sleep, then poll again */
    };
    Kind                                kind  = Kind::Spin;
    std::chrono::steady_clock::duration sleep = {};

    bool operator==(const PollPause &) const = default;
};

/* How long waitFor() polls back to back, and how long it polls with a
 * yield between polls, from the start of the wait */
inline constexpr std::chrono::microseconds kPollSpinWindow{20};
inline constexpr std::chrono::microseconds kPollYieldWindow{200};

/* The longest waitFor() sleeps between two polls */
inline constexpr std::chrono::milliseconds kPollMaxSleep{1};

/* What waitFor() does after a poll that found nothing, elapsed after the
 * wait started. After the yield window, it sleeps for a quarter of the
 * time it has waited, up to kPollMaxSleep. So, apart from however late
 * the OS wakes the thread, a result is noticed at most a quarter of the
 * wait late, and at most kPollMaxSleep late. */
[[nodiscard]] constexpr PollPause
pollPause(std::chrono::steady_clock::duration elapsed) noexcept
{
    if (elapsed < kPollSpinWindow) {
        return {.kind = PollPause::Kind::Spin, .sleep = {}};
    }
    if (elapsed < kPollYieldWindow) {
        return {.kind = PollPause::Kind::Yield, .sleep = {}};
    }
    constexpr int                             kSleepDivisor = 4;
    const std::chrono::steady_clock::duration sleep =
        std::min<std::chrono::steady_clock::duration>(elapsed / kSleepDivisor, kPollMaxSleep);
    return {.kind = PollPause::Kind::Sleep, .sleep = sleep};
}

/* Calls poll until it returns a value, and returns that value, or
 * std::nullopt once timeout has passed. poll returns a std::optional,
 * which is empty while what it's waiting for isn't ready. poll is called
 * at least once, even when timeout is zero, and no sleep is asked to last
 * past the deadline.
 *
 * The time is checked before each poll, not after, so the last poll
 * starts at or after the deadline. A result that's ready by the deadline
 * is never missed, and one that's ready during that last poll is
 * returned, though the deadline has passed. The work did finish, so
 * reporting a timeout instead would fail a transfer that worked.
 *
 * Poll is a template parameter, not a std::function, so a lambda passed
 * here is called directly, with no allocation. */
template <typename Poll>
    requires std::invocable<const Poll &>
[[nodiscard]] std::invoke_result_t<const Poll &>
waitFor(std::chrono::steady_clock::duration timeout, const Poll &poll)
{
    using Clock                      = std::chrono::steady_clock;
    const Clock::time_point start    = Clock::now();
    const Clock::time_point deadline = start + timeout;
    for (Clock::time_point now = start;; now = Clock::now()) {
        const bool expired = now >= deadline;
        if (auto result = poll()) {
            return result;
        }
        if (expired) {
            return std::nullopt;
        }
        const PollPause pause = pollPause(now - start);
        if (pause.kind == PollPause::Kind::Yield) {
            std::this_thread::yield();
        }
        else if (pause.kind == PollPause::Kind::Sleep) {
            std::this_thread::sleep_for(std::min(pause.sleep, deadline - now));
        }
    }
}

} // namespace hipObj
