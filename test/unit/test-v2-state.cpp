/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Unit tests for the v2 client phase machine (src/rdma/v2-state.*). */

#include <gtest/gtest.h>

#include "v2-state.h"

namespace {

using hipObj::v2::Phase;

TEST(V2State, HappyPathGet) {
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

TEST(V2State, IllegalJumpsRejected) {
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

TEST(V2State, NoApplyLeavesPhase) {
  Phase p = Phase::Idle;
  EXPECT_TRUE(hipObj::v2::beginNegotiate(p, false));
  EXPECT_EQ(p, Phase::Idle);
  EXPECT_TRUE(hipObj::v2::beginNegotiate(p));
  EXPECT_TRUE(hipObj::v2::prepareOk(p, false));
  EXPECT_EQ(p, Phase::Negotiating);
}

TEST(V2State, DoubleReadyRejected) {
  Phase p = Phase::Idle;
  hipObj::v2::beginNegotiate(p);
  hipObj::v2::prepareOk(p);
  hipObj::v2::connectOk(p);
  EXPECT_TRUE(hipObj::v2::sendReady(p));
  EXPECT_FALSE(hipObj::v2::sendReady(p)); /* second READY: illegal */
  EXPECT_EQ(p, Phase::Transferring);
}

TEST(V2State, PostExposureFailureDrains) {
  Phase p = Phase::Idle;
  hipObj::v2::beginNegotiate(p);
  /* sendPrepare has been invoked: any failure must drain. */
  EXPECT_TRUE(hipObj::v2::fail(p, /*preExpose=*/false));
  EXPECT_EQ(p, Phase::Draining);
  EXPECT_TRUE(hipObj::v2::drained(p));
  EXPECT_EQ(p, Phase::Idle);
}

TEST(V2State, PreExposureLocalFailureFromIdleIsNoop) {
  Phase p = Phase::Idle;
  EXPECT_FALSE(hipObj::v2::fail(p, /*preExpose=*/true));
  EXPECT_EQ(p, Phase::Idle);
}

TEST(V2State, EveryPhaseFailsToDrainingWhenExposed) {
  for (int i = 1; i <= 5; ++i) {
    Phase p = (Phase)i;
    Phase q = p;
    EXPECT_TRUE(hipObj::v2::fail(q, /*preExpose=*/false));
    EXPECT_EQ(q, Phase::Draining) << "phase " << i;
  }
}

TEST(V2State, ExposedMeansPastIdle) {
  EXPECT_FALSE(hipObj::v2::exposed(Phase::Idle));
  EXPECT_TRUE(hipObj::v2::exposed(Phase::Negotiating));
  EXPECT_TRUE(hipObj::v2::exposed(Phase::Draining));
}

TEST(V2State, PhaseNames) {
  EXPECT_STREQ(hipObj::v2::phaseName(Phase::Idle), "Idle");
  EXPECT_STREQ(hipObj::v2::phaseName(Phase::Transferring), "Transferring");
  EXPECT_STREQ(hipObj::v2::phaseName(Phase::Draining), "Draining");
}

} // namespace
