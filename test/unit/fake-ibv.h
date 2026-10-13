/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Installs a wrapper built from fake verbs as hipObj::ibv(), so tests run
 * without libibverbs or an RDMA device.
 */

#pragma once

#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "ibv-wrapper.h"

namespace hipObjTest {

/* Makes ibv() return a wrapper that calls funcs for as long as it lives,
 * and puts the previous wrapper back when it's destroyed. funcs is used
 * as given: an entry the test leaves null crashes if it's called, rather
 * than reaching the real libibverbs. Create and destroy it only while no
 * other thread is calling ibv(). */
class ScopedFakeIbv {
public:
    explicit ScopedFakeIbv(const hipObj::IbvFuncs &funcs)
        : funcs_(funcs), wrapper_(std::make_unique<hipObj::IBVWrapper>(funcs)),
          previous_(hipObj::setIbvForTest(wrapper_.get()))
    {
    }

    ~ScopedFakeIbv()
    {
        /* Restored in the reverse order they were installed */
        EXPECT_EQ(hipObj::setIbvForTest(previous_), wrapper_.get());
    }

    ScopedFakeIbv(const ScopedFakeIbv &)            = delete;
    ScopedFakeIbv &operator=(const ScopedFakeIbv &) = delete;
    ScopedFakeIbv(ScopedFakeIbv &&)                 = delete;
    ScopedFakeIbv &operator=(ScopedFakeIbv &&)      = delete;

    const hipObj::IbvFuncs &funcs() const noexcept
    {
        return funcs_;
    }

    /* Switches to new fakes, for a test that changes some of its
     * fixture's. The new wrapper is installed before the old one is
     * destroyed, so ibv() never returns a destroyed wrapper. */
    void replace(const hipObj::IbvFuncs &funcs)
    {
        auto wrapper = std::make_unique<hipObj::IBVWrapper>(funcs);
        hipObj::setIbvForTest(wrapper.get());
        wrapper_ = std::move(wrapper);
        funcs_   = funcs;
    }

private:
    hipObj::IbvFuncs                    funcs_;
    std::unique_ptr<hipObj::IBVWrapper> wrapper_;
    hipObj::IBVWrapper                 *previous_;
};

} // namespace hipObjTest
