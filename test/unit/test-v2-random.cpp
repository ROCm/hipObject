/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the v2 randomness policies (PSN generation), PSN
 * delivery into QP transitions, and the public cookie-present
 * plumbing. */

#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "hipobj.h"
#include "ibv-wrapper.h"
#include "v2-random.h"
#include "v2-transport.h"
#include "v2-wire.h"

namespace {

/* Deterministic random source with a scriptable draw sequence. */
class ScriptedRandom : public hipObj::v2::RandomSource {
public:
  std::vector<uint32_t> draws;
  size_t next = 0;
  bool failAll = false;

  void setDraws(std::vector<uint32_t> values) {
    draws = std::move(values);
    next = 0;
  }

  bool next32(uint32_t& out) override {
    if (failAll || next >= draws.size()) {
      return false;
    }
    out = draws[next++];
    return true;
  }
};

class V2RandomTest : public ::testing::Test {
protected:
  void SetUp() override {
    scripted_ = std::make_unique<ScriptedRandom>();
    prev_ = hipObj::v2::setRandomSourceForTest(scripted_.get());
  }
  void TearDown() override {
    hipObj::v2::setRandomSourceForTest(prev_);
  }

  std::unique_ptr<ScriptedRandom> scripted_;
  hipObj::v2::RandomSource* prev_ = nullptr;
};

/* PSN generation: non-zero 24-bit after masking, retry on
 * zero draws, failure on exhausted source. */
TEST_F(V2RandomTest, PsnGeneration) {
  uint32_t psn = 0;
  scripted_->setDraws({0x00000000u, 0x00abcdefu});
  ASSERT_TRUE(hipObj::v2::nextClientPsn(psn));
  EXPECT_EQ(psn, 0x00abcdefu); /* zero draw retried, then accepted */

  scripted_->setDraws({0xffffffffu});
  ASSERT_TRUE(hipObj::v2::nextClientPsn(psn));
  EXPECT_EQ(psn, 0x00ffffffu); /* masked to 24 bits */

  scripted_->setDraws({0u, 0u, 0u});
  EXPECT_FALSE(hipObj::v2::nextClientPsn(psn)); /* all-zero: fail */

  scripted_->failAll = true;
  EXPECT_FALSE(hipObj::v2::nextClientPsn(psn)); /* source failure */
}

/* Cookies pass through unmasked (0 is a valid cookie). */
TEST_F(V2RandomTest, CookiePassThrough) {
  uint32_t cookie = 99;
  scripted_->setDraws({0u});
  ASSERT_TRUE(hipObj::v2::nextCookie(cookie));
  EXPECT_EQ(cookie, 0u);
  scripted_->setDraws({0xdeadbeefu});
  ASSERT_TRUE(hipObj::v2::nextCookie(cookie));
  EXPECT_EQ(cookie, 0xdeadbeefu);
  scripted_->failAll = true;
  EXPECT_FALSE(hipObj::v2::nextCookie(cookie));
}

/* PSNs reach the RTR/RTS attributes (modify_qp spy). */
class V2PsnDeliveryTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto& funcs = hipObj::ibv.funcsForTest();
    saved_ = funcs;
    funcs.modify_qp = [](struct ibv_qp*, struct ibv_qp_attr* attr, int) -> int {
      if (attr->qp_state == 3) { /* RTS */
        g_sqPsn.push_back(attr->sq_psn);
      } else if (attr->qp_state == 2) { /* RTR */
        g_rqPsn.push_back(attr->rq_psn);
      }
      return 0;
    };
    g_rqPsn.clear();
    g_sqPsn.clear();
  }
  void TearDown() override {
    hipObj::ibv.funcsForTest() = saved_;
  }

  hipObj::IbvFuncs saved_;
  static std::vector<uint32_t> g_rqPsn;
  static std::vector<uint32_t> g_sqPsn;
};

std::vector<uint32_t> V2PsnDeliveryTest::g_rqPsn;
std::vector<uint32_t> V2PsnDeliveryTest::g_sqPsn;

TEST_F(V2PsnDeliveryTest, PsnsReachQpAttrs) {
  hipObj::DeviceHandle dh;
  hipObj::RcConnV2 conn;
  conn.qp = reinterpret_cast<struct ibv_qp*>(0x1);
  union ibv_gid gid;
  std::memset(&gid, 0, sizeof(gid));
  ASSERT_EQ(hipObj::v2::transitionQpToRtrV2(&dh, conn, 77, 0, gid, 0x334455),
            0);
  ASSERT_EQ(hipObj::v2::transitionQpToRtsV2(conn, &dh, 0x667788), 0);
  ASSERT_EQ(g_rqPsn.size(), 1u);
  EXPECT_EQ(g_rqPsn[0], 0x334455u); /* server PSN -> rq_psn */
  ASSERT_EQ(g_sqPsn.size(), 1u);
  EXPECT_EQ(g_sqPsn[0], 0x667788u); /* client PSN -> sq_psn */
}

/* The wire parser surfaces cookie presence; the public reply
 * struct carries it. */
TEST(V2CookiePresenceTest, ParserSetsPresence) {
  hipObj::v2::FinalReply reply;
  const std::string headers = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n"
                              "X-Amz-Rdma-Cookie: 00000000\r\n"
                              "X-Amz-Rdma-Bytes: 0\r\n";
  ASSERT_TRUE(hipObj::v2::parseFinalReply(200, headers, reply));
  EXPECT_TRUE(reply.cookiePresent);
  EXPECT_EQ(reply.cookieEcho, 0u); /* zero cookie is valid */

  /* Missing cookie on a success status fails the parse. */
  const std::string noCookie = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n"
                               "X-Amz-Rdma-Bytes: 0\r\n";
  hipObj::v2::FinalReply reply2;
  EXPECT_FALSE(hipObj::v2::parseFinalReply(200, noCookie, reply2));
  EXPECT_FALSE(reply2.cookiePresent);

  /* Public struct mapping: engine code copies the flag; verify the
   * field exists at the tail and holds the parsed value. */
  hipObjFinalReplyV2_t pub{};
  pub.cookiePresent = reply.cookiePresent ? 1 : 0;
  pub.cookieEcho = reply.cookieEcho;
  EXPECT_EQ(pub.cookiePresent, 1);
  EXPECT_EQ(pub.cookieEcho, 0u);
}

} // namespace
