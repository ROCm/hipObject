/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2-clock.h"

#include <chrono>

namespace hipObj {
namespace v2 {

    namespace {

        class SteadyClock : public ClockSource {
        public:
            TimePoint now() override
            {
                return std::chrono::steady_clock::now();
            }
        };

        SteadyClock  g_defaultClock;
        ClockSource *g_override = nullptr;

    } // namespace

    ClockSource &clockSource()
    {
        return g_override ? *g_override : static_cast<ClockSource &>(g_defaultClock);
    }

    ClockSource *setClockSourceForTest(ClockSource *source)
    {
        ClockSource *previous = g_override;
        g_override            = source;
        return previous;
    }

} // namespace v2
} // namespace hipObj
