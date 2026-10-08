/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Injectable monotonic clock for v2 lifetime policies (retired-ring
 * expiry, session timeouts). Production uses a steady-clock
 * implementation; unit tests install a fake to control time. */

#pragma once

#include <cstdint>

namespace hipObj {
namespace v2 {

    class ClockSource {
    public:
        ClockSource()          = default;
        virtual ~ClockSource() = default;

        /* Copying through a base reference would copy only the base part */
        ClockSource(const ClockSource &)            = delete;
        ClockSource &operator=(const ClockSource &) = delete;
        ClockSource(ClockSource &&)                 = delete;
        ClockSource &operator=(ClockSource &&)      = delete;

        virtual uint64_t nowMs() = 0;
    };

    /* Returns the active clock. Production default unless a test
     * override is installed. */
    ClockSource &clockSource();

    /* Installs a test clock and returns the previously active source
     * (nullptr when the production default was active). Passing nullptr
     * restores the default. Unit tests only. */
    ClockSource *setClockSourceForTest(ClockSource *source);

} // namespace v2
} // namespace hipObj
