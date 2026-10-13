/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the v2 randomness policies (PSN generation), PSN
 * delivery into QP transitions, and the public cookie-present
 * plumbing. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fake-device.h"
#include "fake-ibv.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "v2-random.h"
#include "v2-registry.h"
#include "v2-transport.h"
#include "v2-wire.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* The draws a ScriptedRandom returns, in order, and whether it fails
 * every draw instead */
struct Script {
    std::vector<uint32_t> draws{};
    size_t                next    = 0;
    bool                  failAll = false;

    void setDraws(std::vector<uint32_t> values)
    {
        draws = std::move(values);
        next  = 0;
    }
};

/* Deterministic random source with a scriptable draw sequence. */
class ScriptedRandom : public hipObj::v2::RandomSource {
public:
    explicit ScriptedRandom(Script &script) noexcept : script_(&script)
    {
    }
    ~ScriptedRandom() override = default;

    ScriptedRandom(const ScriptedRandom &)            = delete;
    ScriptedRandom &operator=(const ScriptedRandom &) = delete;
    ScriptedRandom(ScriptedRandom &&)                 = delete;
    ScriptedRandom &operator=(ScriptedRandom &&)      = delete;

    std::optional<uint32_t> next32() const override
    {
        if (script_->failAll || script_->next >= script_->draws.size()) {
            return std::nullopt;
        }
        return script_->draws[script_->next++];
    }

private:
    /* A pointer, so the const next32() can change what it points to */
    Script *script_;
};

class V2RandomTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        prev_ = hipObj::v2::setRandomSourceForTest(&random_);
    }
    void TearDown() override
    {
        hipObj::v2::setRandomSourceForTest(prev_);
    }

    Script                          script_{};
    ScriptedRandom                  random_{script_};
    const hipObj::v2::RandomSource *prev_ = nullptr;
};

/* PSN generation: non-zero 24-bit after masking, retry on
 * zero draws, failure on exhausted source. */
TEST_F(V2RandomTest, PsnGeneration)
{
    script_.setDraws({0x00000000U, 0x00abcdefU});
    EXPECT_EQ(hipObj::v2::nextClientPsn(), 0x00abcdefU); /* zero draw retried, then accepted */

    script_.setDraws({0xffffffffU});
    EXPECT_EQ(hipObj::v2::nextClientPsn(), 0x00ffffffU); /* masked to 24 bits */

    script_.setDraws({0U, 0U, 0U});
    EXPECT_FALSE(hipObj::v2::nextClientPsn()); /* all-zero: fail */

    script_.failAll = true;
    EXPECT_FALSE(hipObj::v2::nextClientPsn()); /* source failure */
}

/* Cookies pass through unmasked (0 is a valid cookie). */
TEST_F(V2RandomTest, CookiePassThrough)
{
    script_.setDraws({0U});
    EXPECT_EQ(hipObj::v2::nextCookie(), 0U);
    script_.setDraws({0xdeadbeefU});
    EXPECT_EQ(hipObj::v2::nextCookie(), 0xdeadbeefU);
    script_.failAll = true;
    EXPECT_FALSE(hipObj::v2::nextCookie());
}

/* PSNs reach the RTR/RTS attributes (modify_qp spy). */
class V2PsnDeliveryTest : public ::testing::Test {
protected:
    /* Records the PSNs, and leaves the test's made-up QP alone */
    class PsnVerbs : public hipObjTest::FakeVerbs {
    public:
        using FakeVerbs::FakeVerbs;

        int modify_qp(struct ibv_qp *, struct ibv_qp_attr *attr, int) const override
        {
            if (attr->qp_state == 3) { /* RTS */
                g_sqPsn.push_back(attr->sq_psn);
            }
            else if (attr->qp_state == 2) { /* RTR */
                g_rqPsn.push_back(attr->rq_psn);
            }
            return 0;
        }

        int query_port(struct ibv_context *, uint8_t, struct ibv_port_attr *a) const override
        {
            hipObj::zeroObject(*a);
            a->active_mtu = IBV_MTU_4096;
            return 0;
        }

        /* The test's RcConnV2 owns a fake QP and destroys it when the
         * test body ends, before TearDown() removes the fakes. */
        int destroy_qp(struct ibv_qp *) const override
        {
            return 0;
        }
    };

    void SetUp() override
    {
        fakeIbv_.emplace(verbs_);
        g_rqPsn.clear();
        g_sqPsn.clear();
    }
    void TearDown() override
    {
        fakeIbv_.reset();
    }

    hipObjTest::FakeDevice                   device_{};
    PsnVerbs                                 verbs_{device_};
    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
    static std::vector<uint32_t>             g_rqPsn;
    static std::vector<uint32_t>             g_sqPsn;
};

HIPOBJ_WARN_NO_EXIT_DTOR_OFF
std::vector<uint32_t> V2PsnDeliveryTest::g_rqPsn;
std::vector<uint32_t> V2PsnDeliveryTest::g_sqPsn;
HIPOBJ_WARN_NO_EXIT_DTOR_ON

TEST_F(V2PsnDeliveryTest, PsnsReachQpAttrs)
{
    hipObj::DeviceHandle dh;
    dh.gidIndex = 0;
    hipObj::RcConnV2 conn;
    conn.qp.reset(reinterpret_cast<struct ibv_qp *>(0x1));
    union ibv_gid gid;
    hipObj::zeroObject(gid);
    ASSERT_EQ(hipObj::v2::transitionQpToRtrV2(dh, conn, 77, gid, 0x334455), 0);
    ASSERT_EQ(hipObj::v2::transitionQpToRtsV2(conn, dh, 0x667788), 0);
    ASSERT_EQ(g_rqPsn.size(), 1U);
    EXPECT_EQ(g_rqPsn[0], 0x334455U); /* server PSN -> rq_psn */
    ASSERT_EQ(g_sqPsn.size(), 1U);
    EXPECT_EQ(g_sqPsn[0], 0x667788U); /* client PSN -> sq_psn */
}

/* The wire parser surfaces cookie presence; the public reply
 * struct carries it. */
TEST(V2CookiePresenceTest, ParserSetsPresence)
{
    const std::string headers = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n"
                                "X-Amz-Rdma-Cookie: 00000000\r\n"
                                "X-Amz-Rdma-Bytes: 0\r\n";
    const auto        reply   = hipObj::v2::parseFinalReply(200, headers);
    ASSERT_TRUE(reply);
    EXPECT_TRUE(reply->cookiePresent);
    EXPECT_EQ(reply->cookieEcho, 0U); /* zero cookie is valid */

    /* Missing cookie on a success status fails the parse. */
    const std::string noCookie = "X-Amz-Rdma-Protocol: hipobj-rc-v2\r\n"
                                 "X-Amz-Rdma-Bytes: 0\r\n";
    EXPECT_FALSE(hipObj::v2::parseFinalReply(200, noCookie));

    /* Public struct mapping: engine code copies the flag; verify the
     * field exists at the tail and holds the parsed value. */
    hipObjFinalReplyV2_t pub{};
    pub.cookiePresent = reply->cookiePresent ? 1 : 0;
    pub.cookieEcho    = reply->cookieEcho;
    EXPECT_EQ(pub.cookiePresent, 1);
    EXPECT_EQ(pub.cookieEcho, 0U);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
