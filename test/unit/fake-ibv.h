/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Installs fake verbs as hipObj::ibv(), so tests run without libibverbs
 * or an RDMA device.
 */

#pragma once

#include <gtest/gtest.h>

#include "ibv-wrapper.h"

namespace hipObjTest {

/* Makes ibv() return verbs for as long as it lives, and puts the previous
 * verbs back when it's destroyed. verbs has to outlive it. Create and
 * destroy it only while no other thread is calling ibv(). */
class ScopedFakeIbv {
public:
    explicit ScopedFakeIbv(const hipObj::Verbs &verbs)
        : verbs_(&verbs), previous_(hipObj::setIbvForTest(&verbs))
    {
    }

    ~ScopedFakeIbv()
    {
        /* Restored in the reverse order they were installed */
        EXPECT_EQ(hipObj::setIbvForTest(previous_), verbs_);
    }

    ScopedFakeIbv(const ScopedFakeIbv &)            = delete;
    ScopedFakeIbv &operator=(const ScopedFakeIbv &) = delete;
    ScopedFakeIbv(ScopedFakeIbv &&)                 = delete;
    ScopedFakeIbv &operator=(ScopedFakeIbv &&)      = delete;

private:
    const hipObj::Verbs *verbs_;
    const hipObj::Verbs *previous_;
};

} // namespace hipObjTest
