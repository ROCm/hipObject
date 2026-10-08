/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the qp conflict-discard procedure and data-phase
 * completion validation. */

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "v2-registry.h"
#include "v2-transport.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* Minimal ibverbs fakes: unique qp numbers, scriptable failures. */
struct FakeState {
    int                   createQpFails  = 0;
    int                   destroyQpFails = 0;
    int                   nextQpn        = 1000;
    int                   destroyQpCalls = 0;
    int                   createQpCalls  = 0;
    std::vector<uint32_t> destroyedQpns;
};

HIPOBJ_WARN_NO_EXIT_DTOR_OFF
FakeState g_state;
HIPOBJ_WARN_NO_EXIT_DTOR_ON

struct ibv_cq *
fakeCreateCq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int)
{
    return reinterpret_cast<struct ibv_cq *>(new int(1));
}

int
fakeDestroyCq(struct ibv_cq *cq)
{
    delete reinterpret_cast<int *>(cq);
    return 0;
}

struct ibv_qp *
fakeCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
{
    ++g_state.createQpCalls;
    if (g_state.createQpFails > 0) {
        --g_state.createQpFails;
        errno = ENOMEM;
        return nullptr;
    }
    auto *qp = new struct ibv_qp();
    std::memset(qp, 0, sizeof(*qp));
    qp->qp_num = static_cast<uint32_t>(++g_state.nextQpn);
    return qp;
}

int
fakeDestroyQp(struct ibv_qp *qp)
{
    ++g_state.destroyQpCalls;
    if (g_state.destroyQpFails > 0) {
        --g_state.destroyQpFails;
        errno = EBUSY;
        return 1;
    }
    g_state.destroyedQpns.push_back(qp->qp_num);
    delete qp;
    return 0;
}

int
fakeModifyQp(struct ibv_qp *, struct ibv_qp_attr *, int)
{
    return 0;
}

int
fakeQueryDevice(struct ibv_context *, struct ibv_device_attr *)
{
    return 0;
}

int
fakeDeallocPd(struct ibv_pd *)
{
    return 0;
}

class IbvFakeInstall {
public:
    IbvFakeInstall()
    {
        auto &funcs        = hipObj::ibv.funcsForTest();
        saved_             = funcs;
        funcs.create_cq    = fakeCreateCq;
        funcs.destroy_cq   = fakeDestroyCq;
        funcs.create_qp    = fakeCreateQp;
        funcs.destroy_qp   = fakeDestroyQp;
        funcs.modify_qp    = fakeModifyQp;
        funcs.query_device = fakeQueryDevice;
        funcs.dealloc_pd   = fakeDeallocPd;
    }
    ~IbvFakeInstall()
    {
        hipObj::ibv.funcsForTest() = saved_;
    }

    IbvFakeInstall(const IbvFakeInstall &)            = delete;
    IbvFakeInstall &operator=(const IbvFakeInstall &) = delete;
    IbvFakeInstall(IbvFakeInstall &&)                 = delete;
    IbvFakeInstall &operator=(IbvFakeInstall &&)      = delete;

private:
    hipObj::IbvFuncs saved_;
};

class V2ConflictTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        g_state  = FakeState{};
        install_ = std::make_unique<IbvFakeInstall>();
        testReg_ = std::make_unique<hipObj::v2::ConnectionRegistry>();
        prevReg_ = hipObj::v2::setRegistryForTest(testReg_.get());
    }
    void TearDown() override
    {
        hipObj::v2::setRegistryForTest(prevReg_);
    }

    /* Builds one live entry through the normal sequence. */
    hipObj::v2::ConnId makeEntry()
    {
        auto &reg = hipObj::v2::registry();
        if (!reg.reserveSlot()) {
            return 0;
        }
        uint64_t                      rid = reg.retired().reserve();
        hipObj::v2::ConnectionEntryV2 entry;
        hipObj::RcConnV2              conn;
        if (hipObj::v2::createRcConnV2(&dh_, conn) != 0) {
            reg.unreserveSlot();
            if (rid) {
                reg.retired().unreserve(rid);
            }
            return 0;
        }
        entry.conn          = std::move(conn);
        entry.device        = &dh_;
        entry.reservationId = rid;
        entry.clientPsn     = 0x42;
        return reg.insert(std::move(entry));
    }

    /* install_ is declared first so it's destroyed last: the registry
     * entries free their fake objects through the fake table. */
    std::unique_ptr<IbvFakeInstall>                 install_;
    std::unique_ptr<hipObj::v2::ConnectionRegistry> testReg_;
    hipObj::v2::ConnectionRegistry                 *prevReg_ = nullptr;
    hipObj::DeviceHandle                            dh_;
};

/* Conflict discard: old qp destroyed and recorded, a fresh qp is
 * created on the same cq, and the entry keeps one reservation. */
TEST_F(V2ConflictTest, DiscardAndRecreate)
{
    auto &reg = hipObj::v2::registry();
    auto  id  = makeEntry();
    ASSERT_NE(id, 0U);
    uint32_t       oldQpn    = 0;
    uint64_t       ridBefore = 0;
    struct ibv_cq *cqBefore  = nullptr;
    reg.withEntry(id, [&](hipObj::v2::ConnectionEntryV2 &e) {
        oldQpn    = e.conn.qpNum;
        ridBefore = e.reservationId;
        cqBefore  = e.conn.cq.get();
    });
    size_t recordedBefore = reg.retired().recordedCount();

    ASSERT_EQ(hipObj::v2::discardAndRecreateQp(id), 0);

    reg.withEntry(id, [&](hipObj::v2::ConnectionEntryV2 &e) {
        EXPECT_NE(e.conn.qp, nullptr);
        EXPECT_NE(e.conn.qpNum, oldQpn);
        EXPECT_EQ(e.conn.cq.get(), cqBefore); /* cq survives */
        EXPECT_NE(e.reservationId, 0U);
        EXPECT_NE(e.reservationId, 0U);
    });
    EXPECT_EQ(g_state.destroyedQpns.size(), 1U);
    EXPECT_EQ(g_state.destroyedQpns[0], oldQpn);
    /* Old tuple recorded against the peer PSN. */
    EXPECT_TRUE(reg.retired().contains(oldQpn, 0x42));
    EXPECT_EQ(reg.retired().recordedCount(), recordedBefore + 1);
    /* Ring accounting: one recorded slot per discard. */
    EXPECT_EQ(reg.retired().reservedCount(), 0U);

    /* Clean teardown still works afterwards. */
    EXPECT_EQ(hipObj::v2::releaseConnection(id), 0);
    EXPECT_EQ(reg.size(), 0U);
}

/* Ring exhausted: busy, entry untouched. */
TEST_F(V2ConflictTest, DiscardRingFullIsBusy)
{
    auto &reg = hipObj::v2::registry();
    auto  id  = makeEntry();
    ASSERT_NE(id, 0U);
    /* The entry already holds one reservation; fill the rest. */
    std::vector<uint64_t> fills;
    while (reg.retired().reservedCount() + reg.retired().recordedCount() < reg.retired().kCapacity) {
        uint64_t r = reg.retired().reserve();
        ASSERT_NE(r, 0U);
        fills.push_back(r);
    }
    EXPECT_EQ(hipObj::v2::discardAndRecreateQp(id), hipObj::v2::kReleaseBusy);
    void *qp = nullptr;
    reg.withEntry(id, [&](hipObj::v2::ConnectionEntryV2 &e) { qp = e.conn.qp.get(); });
    EXPECT_NE(qp, nullptr); /* untouched */
    for (auto r : fills) {
        reg.retired().unreserve(r);
    }
    EXPECT_EQ(hipObj::v2::releaseConnection(id), 0);
}

/* destroy_qp failure keeps the live qp; nothing recorded. */
TEST_F(V2ConflictTest, DiscardDestroyFailureKeepsQp)
{
    auto &reg = hipObj::v2::registry();
    auto  id  = makeEntry();
    ASSERT_NE(id, 0U);
    g_state.destroyQpFails = 1;
    size_t recordedBefore  = reg.retired().recordedCount();
    EXPECT_NE(hipObj::v2::discardAndRecreateQp(id), 0);
    void *qp = nullptr;
    reg.withEntry(id, [&](hipObj::v2::ConnectionEntryV2 &e) { qp = e.conn.qp.get(); });
    EXPECT_NE(qp, nullptr);
    EXPECT_EQ(reg.retired().recordedCount(), recordedBefore);
    EXPECT_EQ(hipObj::v2::releaseConnection(id), 0);
}

/* Recreate failure releases the whole entry: the cq is destroyed
 * through the normal path and the ring stays consistent. */
TEST_F(V2ConflictTest, DiscardRecreateFailureReleases)
{
    auto &reg = hipObj::v2::registry();
    auto  id  = makeEntry();
    ASSERT_NE(id, 0U);
    g_state.createQpFails = 1;
    EXPECT_NE(hipObj::v2::discardAndRecreateQp(id), 0);
    EXPECT_EQ(reg.size(), 0U); /* entry gone */
    EXPECT_EQ(reg.retired().reservedCount(), 0U);
    EXPECT_EQ(reg.pendingReserves(), 0U);
}

/* ---- completion validation -------------------------------------- */

struct WcTestData : public ::testing::Test {
    struct ibv_wc makeWc(enum ibv_wc_opcode op, unsigned int flags, uint32_t immHost)
    {
        struct ibv_wc wc {};
        wc.status   = IBV_WC_SUCCESS;
        wc.opcode   = op;
        wc.wc_flags = flags;
        wc.wr_id    = hipObj::v2::kRecvImm;
        wc.imm_data = htonl(immHost);
        return wc;
    }
};

/* GET data phase: one RECV_RDMA_WITH_IMM carrying the cookie. */
TEST_F(WcTestData, GetCompletion)
{
    auto                      wc = makeWc(IBV_WC_RECV_RDMA_WITH_IMM, IBV_WC_WITH_IMM, 0x1a2b3c4d);
    hipObj::v2::WcExpectation exp{hipObj::v2::WcKind::kGet, hipObj::v2::kRecvImm, 0x1a2b3c4d};
    const char               *reason = nullptr;
    EXPECT_TRUE(hipObj::v2::validateDataCompletion(wc, exp, &reason));
}

/* PUT data phase: RECV plus the immediate cookie. */
TEST_F(WcTestData, PutCompletion)
{
    auto                      wc = makeWc(IBV_WC_RECV, IBV_WC_WITH_IMM, 0x1a2b3c4d);
    hipObj::v2::WcExpectation exp{hipObj::v2::WcKind::kPut, hipObj::v2::kRecvImm, 0x1a2b3c4d};
    const char               *reason = nullptr;
    EXPECT_TRUE(hipObj::v2::validateDataCompletion(wc, exp, &reason));

    /* Missing immediate flag fails. */
    wc.wc_flags = 0;
    EXPECT_FALSE(hipObj::v2::validateDataCompletion(wc, exp, &reason));
}

/* Wrong opcode and cookie mismatches are rejected. */
TEST_F(WcTestData, Mismatches)
{
    const char               *reason = nullptr;
    auto                      wc     = makeWc(IBV_WC_RECV, IBV_WC_WITH_IMM, 0x1a2b3c4d);
    hipObj::v2::WcExpectation get{hipObj::v2::WcKind::kGet, hipObj::v2::kRecvImm, 0x1a2b3c4d};
    EXPECT_FALSE(hipObj::v2::validateDataCompletion(wc, get, &reason));

    auto                      wc2 = makeWc(IBV_WC_RECV_RDMA_WITH_IMM, IBV_WC_WITH_IMM, 0x1a2b3c4d);
    hipObj::v2::WcExpectation bad{hipObj::v2::WcKind::kGet, hipObj::v2::kRecvImm, 0xdeadbeef};
    EXPECT_FALSE(hipObj::v2::validateDataCompletion(wc2, bad, &reason));

    auto wc3   = makeWc(IBV_WC_RECV_RDMA_WITH_IMM, IBV_WC_WITH_IMM, 0x1a2b3c4d);
    wc3.status = IBV_WC_WR_FLUSH_ERR;
    hipObj::v2::WcExpectation ok{hipObj::v2::WcKind::kGet, hipObj::v2::kRecvImm, 0x1a2b3c4d};
    EXPECT_FALSE(hipObj::v2::validateDataCompletion(wc3, ok, &reason));
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
