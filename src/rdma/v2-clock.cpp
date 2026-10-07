/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
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
            TimePoint now() const override
            {
                return std::chrono::steady_clock::now();
            }
        };

        const SteadyClock  g_defaultClock;
        const ClockSource *g_override = nullptr;

    } // namespace

    const ClockSource &clockSource()
    {
        return g_override ? *g_override : static_cast<const ClockSource &>(g_defaultClock);
    }

    const ClockSource *setClockSourceForTest(const ClockSource *source)
    {
        const ClockSource *previous = g_override;
        g_override                  = source;
        return previous;
    }

} // namespace v2
} // namespace hipObj
