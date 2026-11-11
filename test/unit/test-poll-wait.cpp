/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for waitFor() and the schedule of pauses between its polls.
 */

#include <chrono>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "poll-wait.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::PollPause;
using hipObj::pollPause;
using hipObj::waitFor;
using std::chrono::microseconds;
using std::chrono::milliseconds;

/* Long enough that no test reaches it */
constexpr std::chrono::hours kNoTimeout{1};

/* The timeout of PollsAgainAfterTheDeadline, and how long its first poll
 * takes */
constexpr milliseconds kShortTimeout{1};
constexpr milliseconds kSlowPoll = 2 * kShortTimeout;

} // namespace

// ---- The pauses between polls -------------------------------------

TEST(PollPause, SpinsThenYieldsThenSleeps)
{
    constexpr PollPause kSpin{.kind = PollPause::Kind::Spin, .sleep = {}};
    constexpr PollPause kYield{.kind = PollPause::Kind::Yield, .sleep = {}};
    EXPECT_EQ(pollPause(microseconds(0)), kSpin);
    EXPECT_EQ(pollPause(hipObj::kPollSpinWindow - std::chrono::nanoseconds(1)), kSpin);
    EXPECT_EQ(pollPause(hipObj::kPollSpinWindow), kYield);
    EXPECT_EQ(pollPause(hipObj::kPollYieldWindow - std::chrono::nanoseconds(1)), kYield);
    EXPECT_EQ(pollPause(hipObj::kPollYieldWindow).kind, PollPause::Kind::Sleep);
}

TEST(PollPause, SleepsAQuarterOfTheWaitUpToTheLimit)
{
    EXPECT_EQ(pollPause(microseconds(200)).sleep, microseconds(50));
    EXPECT_EQ(pollPause(microseconds(1000)).sleep, microseconds(250));
    EXPECT_EQ(pollPause(milliseconds(4)).sleep, hipObj::kPollMaxSleep);
    EXPECT_EQ(pollPause(std::chrono::hours(24)).sleep, hipObj::kPollMaxSleep);
}

TEST(PollPause, SleepNeverShrinks)
{
    std::chrono::steady_clock::duration last{};
    for (microseconds elapsed = hipObj::kPollYieldWindow; elapsed <= milliseconds(10);
         elapsed += microseconds(7)) {
        const PollPause pause = pollPause(elapsed);
        ASSERT_EQ(pause.kind, PollPause::Kind::Sleep);
        EXPECT_GT(pause.sleep, std::chrono::steady_clock::duration::zero());
        EXPECT_GE(pause.sleep, last);
        last = pause.sleep;
    }
}

// ---- Waiting --------------------------------------------------------

TEST(WaitFor, ReturnsTheFirstResult)
{
    int calls = 0;
    EXPECT_EQ(waitFor(kNoTimeout, [&calls] { return std::optional<int>(++calls); }), 1);
    EXPECT_EQ(calls, 1);
}

TEST(WaitFor, PollsUntilThereIsAResult)
{
    constexpr int kNotReady = 3;
    int           calls     = 0;
    const auto    poll      = [&calls]() -> std::optional<int> {
        if (++calls <= kNotReady) {
            return std::nullopt;
        }
        return calls;
    };
    EXPECT_EQ(waitFor(kNoTimeout, poll), kNotReady + 1);
    EXPECT_EQ(calls, kNotReady + 1);
}

/* Long enough to get through the spinning and the yielding, and some
 * sleeps */
TEST(WaitFor, TimesOutWithoutAResult)
{
    constexpr milliseconds kTimeout{10};
    int                    calls = 0;
    const auto             poll  = [&calls]() -> std::optional<int> {
        ++calls;
        return std::nullopt;
    };
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(waitFor(kTimeout, poll), std::nullopt);
    EXPECT_GE(std::chrono::steady_clock::now() - start, kTimeout);
    EXPECT_GE(calls, 2);
}

/* A result that's ready when the deadline passes isn't missed: the poll
 * that runs past the deadline is followed by one more */
TEST(WaitFor, PollsAgainAfterTheDeadline)
{
    int        calls = 0;
    const auto poll  = [&calls]() -> std::optional<int> {
        if (++calls == 1) {
            /* The result becomes ready while the first poll runs, which
             * takes longer than the whole timeout */
            std::this_thread::sleep_for(kSlowPoll);
            return std::nullopt;
        }
        return calls;
    };
    EXPECT_EQ(waitFor(kShortTimeout, poll), 2);
}

TEST(WaitFor, PollsOnceWithNoTime)
{
    int        calls = 0;
    const auto poll  = [&calls]() -> std::optional<int> {
        ++calls;
        return std::nullopt;
    };
    EXPECT_EQ(waitFor(milliseconds(0), poll), std::nullopt);
    EXPECT_EQ(calls, 1);
}
