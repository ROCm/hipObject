/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Unit tests for the v2 client phase machine (src/rdma/v2-state.*). */

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "v2-state.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::v2::Phase;

TEST(V2State, HappyPathGet)
{
    Phase p = Phase::Idle;
    EXPECT_TRUE(hipObj::v2::beginNegotiate(p));
    EXPECT_EQ(p, Phase::Negotiating);
    EXPECT_TRUE(hipObj::v2::prepareOk(p));
    EXPECT_EQ(p, Phase::Connecting);
    EXPECT_TRUE(hipObj::v2::connectOk(p));
    EXPECT_EQ(p, Phase::Ready);
    EXPECT_TRUE(hipObj::v2::sendReady(p));
    EXPECT_EQ(p, Phase::Transferring);
    EXPECT_TRUE(hipObj::v2::transferDone(p));
    EXPECT_EQ(p, Phase::Draining);
    EXPECT_TRUE(hipObj::v2::drained(p));
    EXPECT_EQ(p, Phase::Idle);
}

TEST(V2State, IllegalJumpsRejected)
{
    Phase p = Phase::Idle;
    EXPECT_FALSE(hipObj::v2::prepareOk(p));    /* Idle -> Connecting */
    EXPECT_FALSE(hipObj::v2::connectOk(p));    /* Idle -> Ready */
    EXPECT_FALSE(hipObj::v2::sendReady(p));    /* Idle -> Transferring */
    EXPECT_FALSE(hipObj::v2::transferDone(p)); /* Idle -> Draining */
    EXPECT_FALSE(hipObj::v2::drained(p));      /* Idle -> Idle via drain */
    EXPECT_EQ(p, Phase::Idle);                 /* nothing applied */

    EXPECT_TRUE(hipObj::v2::beginNegotiate(p));
    EXPECT_FALSE(hipObj::v2::connectOk(p)); /* skip Connecting */
    EXPECT_FALSE(hipObj::v2::sendReady(p)); /* skip Transferring entry */
    EXPECT_EQ(p, Phase::Negotiating);
}

TEST(V2State, NoApplyLeavesPhase)
{
    Phase p = Phase::Idle;
    EXPECT_TRUE(hipObj::v2::beginNegotiate(p, false));
    EXPECT_EQ(p, Phase::Idle);
    EXPECT_TRUE(hipObj::v2::beginNegotiate(p));
    EXPECT_TRUE(hipObj::v2::prepareOk(p, false));
    EXPECT_EQ(p, Phase::Negotiating);
}

TEST(V2State, DoubleReadyRejected)
{
    Phase p = Phase::Idle;
    hipObj::v2::beginNegotiate(p);
    hipObj::v2::prepareOk(p);
    hipObj::v2::connectOk(p);
    EXPECT_TRUE(hipObj::v2::sendReady(p));
    EXPECT_FALSE(hipObj::v2::sendReady(p)); /* second READY: illegal */
    EXPECT_EQ(p, Phase::Transferring);
}

TEST(V2State, PostExposureFailureDrains)
{
    Phase p = Phase::Idle;
    hipObj::v2::beginNegotiate(p);
    /* sendPrepare has been invoked: any failure must drain. */
    EXPECT_TRUE(hipObj::v2::fail(p, /*preExpose=*/false));
    EXPECT_EQ(p, Phase::Draining);
    EXPECT_TRUE(hipObj::v2::drained(p));
    EXPECT_EQ(p, Phase::Idle);
}

TEST(V2State, PreExposureLocalFailureFromIdleIsNoop)
{
    Phase p = Phase::Idle;
    EXPECT_FALSE(hipObj::v2::fail(p, /*preExpose=*/true));
    EXPECT_EQ(p, Phase::Idle);
}

TEST(V2State, EveryPhaseFailsToDrainingWhenExposed)
{
    for (int i = 1; i <= 5; ++i) {
        Phase p = static_cast<Phase>(i);
        Phase q = p;
        EXPECT_TRUE(hipObj::v2::fail(q, /*preExpose=*/false));
        EXPECT_EQ(q, Phase::Draining) << "phase " << i;
    }
}

TEST(V2State, ExposedMeansPastIdle)
{
    EXPECT_FALSE(hipObj::v2::exposed(Phase::Idle));
    EXPECT_TRUE(hipObj::v2::exposed(Phase::Negotiating));
    EXPECT_TRUE(hipObj::v2::exposed(Phase::Draining));
}

TEST(V2State, PhaseNames)
{
    EXPECT_STREQ(hipObj::v2::phaseName(Phase::Idle), "Idle");
    EXPECT_STREQ(hipObj::v2::phaseName(Phase::Negotiating), "Negotiating");
    EXPECT_STREQ(hipObj::v2::phaseName(Phase::Connecting), "Connecting");
    EXPECT_STREQ(hipObj::v2::phaseName(Phase::Ready), "Ready");
    EXPECT_STREQ(hipObj::v2::phaseName(Phase::Transferring), "Transferring");
    EXPECT_STREQ(hipObj::v2::phaseName(Phase::Draining), "Draining");

    /* A value no enumerator has, as a corrupted phase would hold. Copy it
     * in rather than cast it, which static analysis flags. */
    const uint8_t raw = 6;
    Phase         unknown;
    static_assert(sizeof(unknown) == sizeof(raw));
    std::memcpy(&unknown, &raw, sizeof(unknown));
    EXPECT_STREQ(hipObj::v2::phaseName(unknown), "?");
}

TEST(V2State, NegotiationStartsOnlyFromIdle)
{
    for (int i = 1; i <= 5; ++i) {
        Phase p = static_cast<Phase>(i);
        EXPECT_FALSE(hipObj::v2::beginNegotiate(p)) << "phase " << i;
        EXPECT_EQ(p, static_cast<Phase>(i));
    }
}

TEST(V2State, PreExposureFailureReturnsToIdle)
{
    Phase p = Phase::Negotiating;
    EXPECT_TRUE(hipObj::v2::fail(p, /*preExpose=*/true, /*apply=*/false));
    EXPECT_EQ(p, Phase::Negotiating);
    EXPECT_TRUE(hipObj::v2::fail(p, /*preExpose=*/true));
    EXPECT_EQ(p, Phase::Idle);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
