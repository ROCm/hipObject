/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Injectable monotonic clock for v2 lifetime policies (retired-ring
 * expiry, session timeouts). Production uses a steady-clock
 * implementation; unit tests install a fake to control time. */

#pragma once

#include <chrono>

namespace hipObj {
namespace v2 {

    /* A time from a ClockSource. It's a steady-clock time point because
     * some callers wait for a deadline from clockSource() on the real
     * steady clock (a condition variable, a CQ poll). */
    using TimePoint = std::chrono::steady_clock::time_point;

    /* The methods are const, so one clock can be shared by everything
     * that uses it */
    class ClockSource {
    public:
        ClockSource()          = default;
        virtual ~ClockSource() = default;

        /* Copying through a reference to the base would slice */
        ClockSource(const ClockSource &)            = delete;
        ClockSource &operator=(const ClockSource &) = delete;
        ClockSource(ClockSource &&)                 = delete;
        ClockSource &operator=(ClockSource &&)      = delete;

        virtual TimePoint now() const = 0;
    };

    /* Returns the active clock. Production default unless a test
     * override is installed. */
    const ClockSource &clockSource();

    /* Installs a test clock and returns the previously active source
     * (nullptr when the production default was active). Passing nullptr
     * restores the default. Unit tests only. */
    const ClockSource *setClockSourceForTest(const ClockSource *source);

} // namespace v2
} // namespace hipObj
