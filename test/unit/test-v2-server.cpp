/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the server-side v2 request parsers and session
 * table state machine (no RDMA hardware; transitions only). */

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "v2-clock.h"
#include "v2_request.h"
#include "v2_session.h"

namespace {

using hipObj::v2::SessState;

/* Scriptable clock so deadline arithmetic is deterministic. */
class FakeClock : public hipObj::v2::ClockSource {
public:
  uint64_t now = 1000;
  uint64_t nowMs() override {
    return now;
  }
};

class V2SessionTest : public ::testing::Test {
protected:
  void SetUp() override {
    clock_ = new FakeClock;
    prevClock_ = hipObj::v2::setClockSourceForTest(clock_);
  }
  void TearDown() override {
    hipObj::v2::setClockSourceForTest(prevClock_);
    delete clock_;
  }

  hipObj::v2::V2Session makeSession(const std::string& id) {
    hipObj::v2::V2Session s;
    s.id = id;
    s.op = "GET";
    s.target = "/bucket/key";
    s.size = 4096;
    s.cookie = 0x1a2b3c4d;
    s.clientPsn = 7;
    s.accessKey = "AKIA-test";
    s.clientDeadlineAt = clock_->now + 10000;
    return s;
  }

  FakeClock* clock_;
  hipObj::v2::ClockSource* prevClock_;
  hipObj::v2::SessionTable table_;
};

/* Insert sets ioActive=1 atomically with publication. */
TEST_F(V2SessionTest, InsertPublishesWithIoActive) {
  EXPECT_TRUE(table_.insert(makeSession("aa")));
  table_.withSession("aa", [](hipObj::v2::V2Session& s) {
    EXPECT_EQ(s.ioActive, 1);
    EXPECT_EQ(s.state, SessState::Prepared);
  });
  /* Duplicate id rejected. */
  EXPECT_FALSE(table_.insert(makeSession("aa")));
}

/* PREPARE success: Publishing -> Prepared with a re-armed
 * client deadline measured from the send completion. */
TEST_F(V2SessionTest, PublishingRoundTripRearmsDeadline) {
  table_.insert(makeSession("bb"));
  EXPECT_TRUE(table_.beginPublishing("bb"));
  EXPECT_EQ(table_.stateOf("bb"), SessState::Publishing);
  clock_->now += 4000; /* slow response path */
  EXPECT_TRUE(table_.finishPublishing("bb", 10000));
  EXPECT_EQ(table_.stateOf("bb"), SessState::Prepared);
  table_.withSession("bb", [this](hipObj::v2::V2Session& s) {
    EXPECT_EQ(s.clientDeadlineAt, clock_->now + 10000);
  });
}

/* finishPublishing only applies while still Publishing. */
TEST_F(V2SessionTest, FinishPublishingRejectedOutsidePublishing) {
  table_.insert(makeSession("cc"));
  EXPECT_FALSE(table_.finishPublishing("cc", 10000));
}

/* READY transitions Prepared -> Transferring and arms T_exec;
 * an expired session is moved to Reaping instead (no revival). */
TEST_F(V2SessionTest, ReadyTransitionsAndExpiryCheck) {
  table_.insert(makeSession("dd"));
  EXPECT_TRUE(table_.beginTransferring("dd", 30000));
  EXPECT_EQ(table_.stateOf("dd"), SessState::Transferring);

  /* Second READY while Transferring does not transition. */
  EXPECT_FALSE(table_.beginTransferring("dd", 30000));

  /* Expired: deadline passes, READY must not revive. */
  table_.insert(makeSession("ee"));
  table_.withSession("ee", [this](hipObj::v2::V2Session& s) {
    s.clientDeadlineAt = clock_->now - 1;
  });
  EXPECT_FALSE(table_.beginTransferring("ee", 30000));
  EXPECT_EQ(table_.stateOf("ee"), SessState::Reaping);
}

/* FINAL confirmation: Transferring -> Completing. */
TEST_F(V2SessionTest, CompletingTransition) {
  table_.insert(makeSession("ff"));
  ASSERT_TRUE(table_.beginTransferring("ff", 30000));
  EXPECT_TRUE(table_.beginCompleting("ff"));
  EXPECT_EQ(table_.stateOf("ff"), SessState::Completing);
  /* Only from Transferring. */
  table_.insert(makeSession("gg"));
  EXPECT_FALSE(table_.beginCompleting("gg"));
}

/* toReaping is idempotent and refuses unknown ids. */
TEST_F(V2SessionTest, ToReapingIdempotent) {
  table_.insert(makeSession("hh"));
  EXPECT_TRUE(table_.toReaping("hh"));
  EXPECT_FALSE(table_.toReaping("hh"));
  EXPECT_FALSE(table_.toReaping("zz"));
  EXPECT_EQ(table_.stateOf("hh"), SessState::Reaping);
}

/* Destroy gate: single claim, partial destroy poisons, full
 * destroy erases; retry path mirrors the client registry. */
TEST_F(V2SessionTest, DestroyGateLifecycle) {
  table_.insert(makeSession("ii"));
  table_.toReaping("ii");
  /* Fake live objects. */
  table_.withSession("ii", [](hipObj::v2::V2Session& s) {
    s.qp = reinterpret_cast<struct ibv_qp*>(0x11);
    s.cq = reinterpret_cast<struct ibv_cq*>(0x22);
  });
  EXPECT_TRUE(table_.claimDestroy("ii"));
  /* Second claim while destroying is rejected. */
  EXPECT_FALSE(table_.claimDestroy("ii"));
  /* Partial failure: qp destroyed, cq left. */
  table_.commitDestroy("ii", true, false);
  table_.withSession("ii", [](hipObj::v2::V2Session& s) {
    EXPECT_EQ(s.qp, nullptr);
    EXPECT_NE(s.cq, nullptr);
    EXPECT_TRUE(s.poisoned);
    EXPECT_FALSE(s.destroying);
  });
  /* Poisoned entries are re-claimable exactly once more. */
  EXPECT_TRUE(table_.claimDestroy("ii"));
  table_.commitDestroy("ii", false, true);
  EXPECT_EQ(table_.size(), 0u);
}

/* claimDestroy requires Reaping state. */
TEST_F(V2SessionTest, ClaimRequiresReaping) {
  table_.insert(makeSession("jj"));
  EXPECT_FALSE(table_.claimDestroy("jj"));
  table_.toReaping("jj");
  EXPECT_TRUE(table_.claimDestroy("jj"));
}

/* awaitNotPublishing observes the exit transition. */
TEST_F(V2SessionTest, AwaitNotPublishing) {
  table_.insert(makeSession("kk"));
  table_.beginPublishing("kk");
  uint64_t deadline = clock_->now + 5000;
  /* Transition from another "thread" - simulated inline before the
   * wait by finishing early: emulate via direct finish. */
  table_.finishPublishing("kk", 10000);
  EXPECT_EQ(table_.awaitNotPublishing("kk", deadline), SessState::Prepared);
}

/* awaitNotPublishing on an erased id reports Reaping. */
TEST_F(V2SessionTest, AwaitOnErasedId) {
  table_.insert(makeSession("ll"));
  table_.toReaping("ll");
  table_.withSession("ll", [](hipObj::v2::V2Session& s) {
    s.qp = nullptr;
    s.cq = nullptr;
  });
  table_.claimDestroy("ll");
  table_.commitDestroy("ll", true, true);
  EXPECT_EQ(table_.size(), 0u);
  EXPECT_EQ(table_.awaitNotPublishing("ll", clock_->now + 100),
            SessState::Reaping);
}

/* ---- request parsers ------------------------------------------- */

std::map<std::string, std::string> hdrs(
  std::initializer_list<std::pair<const std::string, std::string>> v) {
  return std::map<std::string, std::string>(v);
}

TEST(V2RequestParser, PrepareHappyPath) {
  auto h = hdrs({
    {"x-amz-rdma-protocol", "hipobj-rc-v2"},
    {"x-amz-rdma-token", std::string(88, 'a')},
    {"x-amz-rdma-psn", "00ff10"},
    {"x-amz-rdma-cookie", "1a2b3c4d"},
    {"x-amz-rdma-op", "GET"},
    {"x-amz-rdma-target", "/bucket/key?partNumber=1"},
    {"x-amz-rdma-size", "4096"},
    {"x-amz-rdma-offset", "1024"},
  });
  const std::string raw =
    "Host: gw\r\n"
    "Authorization: AWS4-HMAC-SHA256 Credential=AKIA/x\r\n"
    "X-Amz-Rdma-Token: " +
    std::string(88, 'a') + "\r\n";
  auto req = hipObj::v2::parsePrepareRequest(h, raw);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->clientPsn, 0x00ff10u);
  EXPECT_EQ(req->cookie, 0x1a2b3c4du);
  EXPECT_EQ(req->op, "GET");
  EXPECT_TRUE(req->hasOffset);
  EXPECT_EQ(req->offset, 1024u);
  EXPECT_EQ(req->authorization, "AWS4-HMAC-SHA256 Credential=AKIA/x");
}

TEST(V2RequestParser, PrepareRejectsBadFields) {
  const std::string raw = "Authorization: sig\r\n";
  /* Missing protocol. */
  auto h1 = hdrs({{"x-amz-rdma-token", std::string(88, 'a')},
                  {"x-amz-rdma-psn", "000001"},
                  {"x-amz-rdma-cookie", "1a2b3c4d"},
                  {"x-amz-rdma-op", "GET"},
                  {"x-amz-rdma-target", "/k"},
                  {"x-amz-rdma-size", "1"}});
  EXPECT_FALSE(hipObj::v2::parsePrepareRequest(h1, raw).has_value());
  /* PSN zero. */
  auto h2 = h1;
  h2["x-amz-rdma-protocol"] = "hipobj-rc-v2";
  h2["x-amz-rdma-psn"] = "000000";
  EXPECT_FALSE(hipObj::v2::parsePrepareRequest(h2, raw).has_value());
  /* Size over the 2GiB cap parses fine; the handler rejects it
   * with 413 (covered by the handler-level tests). */
  auto h3 = h2;
  h3["x-amz-rdma-psn"] = "000001";
  h3["x-amz-rdma-size"] = "2147483648";
  EXPECT_TRUE(hipObj::v2::parsePrepareRequest(h3, raw).has_value());
}

TEST(V2RequestParser, ReadyAndCancel) {
  const std::string raw = "Authorization: AWS4 sig\r\n";
  auto h = hdrs({
    {"x-amz-rdma-protocol", "hipobj-rc-v2"},
    {"x-amz-rdma-session", std::string(32, 'f')},
    {"x-amz-rdma-cookie", "00000001"},
  });
  auto r = hipObj::v2::parseReadyRequest(h, raw);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->session, std::string(32, 'f'));
  auto c = hipObj::v2::parseCancelRequest(
    hdrs({{"x-amz-rdma-protocol", "hipobj-rc-v2"},
          {"x-amz-rdma-session", std::string(32, 'f')}}),
    raw);
  ASSERT_TRUE(c.has_value());
  /* Short session id rejected. */
  auto bad = hdrs({{"x-amz-rdma-protocol", "hipobj-rc-v2"},
                   {"x-amz-rdma-session", "abcd"},
                   {"x-amz-rdma-cookie", "00000001"}});
  EXPECT_FALSE(hipObj::v2::parseReadyRequest(bad, raw).has_value());
}

} // namespace
