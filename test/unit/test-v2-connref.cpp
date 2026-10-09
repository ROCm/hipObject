/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Connection-reference fault test through the real handler path:
 * onPrepare() fails INIT, the first destroy fails, the session
 * keeps the surviving QP with its reference, and reapSession()
 * retries destroy and consumes the reference exactly once. */

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../../../src/common/ibv-core.h"
#include "../../../src/common/ibv-ptr.h"
#include "../../../src/common/ibv-wrapper.h"
#include "../../../src/rdma/token.h"
#include "../../../src/rdma/v2-registry.h"
#include "../../../src/rdma/v2-transport.h"
#include "hipobj-warnings.h"
#include "malloc_ptr.h"
#include "v2_backend.h"
#include "v2_handlers.h"
#include "v2_request.h"
#include "v2_session.h"
#include "v2_sigv4.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

struct FaultCtl {
    int initFails       = 0;
    int destroyQpFails  = 0;
    int destroyQpCalls  = 0;
    int destroyCqCalls  = 0;
    int deregMrCalls    = 0;
    int createQpCalls   = 0;
    int openDeviceCalls = 0;
    /* Non-zero base refs to catch a double release. */
    unsigned baseRefs = 3;
};

FaultCtl g_fault;

struct FakeCq {
    int magic = 0x4351;
};
struct FakeCtx {
    int magic = 0x4358;
};
struct FakeDev {
    int magic = 0x4445;
};
struct FakePd {
    int magic = 0x5044;
};

int
fakeModifyQp(struct ibv_qp *, struct ibv_qp_attr *attr, int)
{
    if (attr->qp_state == IBV_QPS_INIT && g_fault.initFails > 0) {
        --g_fault.initFails;
        errno = EINVAL;
        return 1;
    }
    return 0;
}

int
fakeDestroyQp(struct ibv_qp *qp)
{
    ++g_fault.destroyQpCalls;
    if (g_fault.destroyQpFails > 0) {
        --g_fault.destroyQpFails;
        errno = EBUSY;
        return 1;
    }
    delete qp;
    return 0;
}

int
fakeDestroyCq(struct ibv_cq *cq)
{
    ++g_fault.destroyCqCalls;
    delete reinterpret_cast<FakeCq *>(cq);
    return 0;
}

struct ibv_qp *
fakeCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
{
    ++g_fault.createQpCalls;
    /* A real (zeroed) ibv_qp: the transport reads qp->qp_num. */
    auto *qp   = new struct ibv_qp();
    qp->qp_num = 0x77;
    return qp;
}

/* A real (zeroed) ibv_mr: the PREPARE reply reads addr and rkey. */
struct ibv_mr *
fakeRegMrIova2(struct ibv_pd *, void *addr, size_t length, uintptr_t, int)
{
    auto *mr   = new struct ibv_mr();
    mr->addr   = addr;
    mr->length = length;
    mr->rkey   = 0x5678;
    return mr;
}

struct ibv_mr *
fakeRegMr(struct ibv_pd *pd, void *addr, size_t length, int access)
{
    return fakeRegMrIova2(pd, addr, length, reinterpret_cast<uintptr_t>(addr), access);
}

int
fakeDeregMr(struct ibv_mr *mr)
{
    ++g_fault.deregMrCalls;
    delete mr;
    return 0;
}

struct ibv_cq *
fakeCreateCq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int)
{
    return reinterpret_cast<struct ibv_cq *>(new FakeCq());
}

struct ibv_context *
fakeOpenDevice(struct ibv_device *)
{
    ++g_fault.openDeviceCalls;
    return reinterpret_cast<struct ibv_context *>(new FakeCtx());
}

struct ibv_device **
fakeGetDeviceList(int *n)
{
    *n = 1;
    /* NULL-terminated, like the real ibv_get_device_list(); allocated
     * as an array to match the delete[] in fakeFreeDeviceList(). */
    auto *dev = reinterpret_cast<struct ibv_device *>(new FakeDev());
    return new struct ibv_device *[2]{dev, nullptr};
}

void
fakeFreeDeviceList(struct ibv_device **list)
{
    delete reinterpret_cast<FakeDev *>(*list);
    delete[] list;
}

struct ibv_pd *
fakeAllocPd(struct ibv_context *)
{
    return reinterpret_cast<struct ibv_pd *>(new FakePd());
}

int
fakeDeallocPd(struct ibv_pd *pd)
{
    delete reinterpret_cast<FakePd *>(pd);
    return 0;
}

int
fakeQueryDevice(struct ibv_context *, struct ibv_device_attr *a)
{
    std::memset(a, 0, sizeof(*a));
    return 0;
}

int
fakeQueryPort(struct ibv_context *, uint8_t, struct ibv_port_attr *a)
{
    std::memset(a, 0, sizeof(*a));
    a->active_mtu = IBV_MTU_4096;
    return 0;
}

int
fakeCloseDevice(struct ibv_context *ctx)
{
    delete reinterpret_cast<FakeCtx *>(ctx);
    return 0;
}

int
fakeQueryGid(struct ibv_context *, uint8_t, int, union ibv_gid *g)
{
    std::memset(g, 0, sizeof(*g));
    return 0;
}

/* Frees the fake objects a test's handlers gave up on, skipping any a
 * fake destroy verb already freed, so a regression fails the test's
 * checks instead of double-freeing here. The test sets destroyQpFails,
 * so fakeDestroyQp() never frees the QP. */
void
freeAbandonedFakes(struct ibv_qp *qp, struct ibv_cq *cq, struct ibv_mr *stagingMr, void *staging)
{
    delete qp;
    if (g_fault.destroyCqCalls == 0) {
        delete reinterpret_cast<FakeCq *>(cq);
    }
    if (g_fault.deregMrCalls == 0) {
        delete stagingMr;
        hipObj::FreeDeleter{}(staging);
    }
}

/* Verifier stub: every request passes with an all-headers signed
 * list so the fault test reaches the transport path. */
class PassAllVerifier : public hipObj::v2::SigV4Verifier {
public:
    std::optional<hipObj::v2::VerifiedCredential> verify(const std::string &, const std::string &,
                                                         const std::string &, const std::string &) override
    {
        return hipObj::v2::VerifiedCredential{.accessKey = "k", .signedHeaders = "*"};
    }
};

class ConnRefFaultTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        saved_             = hipObj::ibv.funcsForTest();
        auto &f            = hipObj::ibv.funcsForTest();
        f.modify_qp        = fakeModifyQp;
        f.destroy_qp       = fakeDestroyQp;
        f.destroy_cq       = fakeDestroyCq;
        f.create_qp        = fakeCreateQp;
        f.create_cq        = fakeCreateCq;
        f.open_device      = fakeOpenDevice;
        f.get_device_list  = fakeGetDeviceList;
        f.free_device_list = fakeFreeDeviceList;
        f.alloc_pd         = fakeAllocPd;
        f.dealloc_pd       = fakeDeallocPd;
        f.query_port       = fakeQueryPort;
        f.query_device     = fakeQueryDevice;
        f.close_device     = fakeCloseDevice;
        f.query_gid        = fakeQueryGid;
        f.reg_mr           = fakeRegMr;
        f.reg_mr_iova2     = fakeRegMrIova2;
        f.dereg_mr         = fakeDeregMr;
        g_fault            = FaultCtl{};
        verifier_          = std::make_unique<PassAllVerifier>();
        backend_           = std::make_unique<hipObj::v2::MemoryBackend>();
    }
    void TearDown() override
    {
        hipObj::ibv.funcsForTest() = saved_;
    }

    /* A PUT PREPARE that reaches the transport path */
    static hipObj::v2::PrepareRequest makePutPrepare()
    {
        hipObj::v2::PrepareRequest req;
        req.protocol = "hipobj-rc-v2";
        /* Real encoded token: RC transport, nonzero GID, so the server's
           semantic checks accept it. */
        hipObj::RdmaToken peerTokEnc{};
        peerTokEnc.qpNum = 0x1234;
        peerTokEnc.gid.fill(0xab);
        peerTokEnc.transport = hipObj::TRANSPORT_RC;
        peerTokEnc.portNum   = 1;
        req.token            = hipObj::encodeRdmaToken(peerTokEnc);
        req.clientPsn        = 0x0a1b2c;
        req.cookie           = 0x1a2b3c4d;
        req.op               = "PUT";
        req.target           = "/bucket/obj";
        req.size             = 64;
        req.authorization    = "AWS4-HMAC-SHA256 stub";
        return req;
    }

    hipObj::IbvFuncs                           saved_;
    std::unique_ptr<hipObj::v2::SigV4Verifier> verifier_;
    std::unique_ptr<hipObj::v2::MemoryBackend> backend_;
};

/* INIT fails inside onPrepare and the first destroy fails: the
 * session must hold the surviving QP and keep the reference; a
 * reaper retry destroys it and consumes the reference exactly
 * once against a non-zero base. */
TEST_F(ConnRefFaultTest, InitAndDestroyFailureKeepsOwnership)
{
    hipObj::v2::ControlHandlers handlers(verifier_.get(), backend_.get(), hipObj::v2::ServerConfig{});
    g_fault.initFails      = 1;
    g_fault.destroyQpFails = 1;

    auto r = handlers.onPrepare(makePutPrepare(), "");
    ASSERT_EQ(r.status, 500) << "INIT failure must answer 500";

    auto ids = handlers.table().ids();
    ASSERT_EQ(ids.size(), 1U) << "exactly the failed session remains";
    std::string sid = ids[0];

    bool                  sawSurvivingQp = false;
    bool                  sawConnRefHeld = false;
    hipObj::DeviceHandle *dev            = nullptr;
    handlers.table().withSession(sid, [&](hipObj::v2::V2Session &s) {
        sawSurvivingQp = s.conn.qp != nullptr;
        sawConnRefHeld = s.connRefHeld;
        dev            = s.device;
    });
    ASSERT_TRUE(sawSurvivingQp);
    ASSERT_TRUE(sawConnRefHeld);
    ASSERT_NE(dev, nullptr);

    /* Reference ledger against a non-zero base: creation raised the
     * counter; the failed destroys must have left it there. */
    const uint32_t base = dev->connRefs.load();
    EXPECT_GT(base, 0U) << "creation must have raised the counter";

    /* Reaper retry: the QP destroys cleanly this time and the
     * reference is consumed exactly once. */
    handlers.reapSession(sid);
    EXPECT_EQ(dev->connRefs.load(), base - 1U) << "successful retry consumes exactly one reference";

    /* A further reap finds no session - the counter must not move
     * (no double release through the CQ-only path). */
    handlers.reapSession(sid);
    EXPECT_EQ(dev->connRefs.load(), base - 1U) << "no additional release after the session is gone";

    EXPECT_EQ(g_fault.destroyQpCalls, 2);
}

/* A session the handlers can't reap when they're destroyed (here, one
 * whose PREPARE response never finished sending) still holds a live QP
 * and registered staging. Member destruction must not tear those down:
 * its deleters ignore failure, so the staging could be freed under a
 * QP that might still be in use. The handlers give them up instead. */
TEST_F(ConnRefFaultTest, DestructionRelinquishesUnreapedTransport)
{
    struct ibv_qp *qp        = nullptr;
    struct ibv_cq *cq        = nullptr;
    struct ibv_mr *stagingMr = nullptr;
    void          *staging   = nullptr;
    {
        hipObj::v2::ControlHandlers handlers(verifier_.get(), backend_.get(), hipObj::v2::ServerConfig{});
        g_fault.destroyQpFails = 1000; /* the QP may still be in use */
        ASSERT_EQ(handlers.onPrepare(makePutPrepare(), "").status, 200);
        auto ids = handlers.table().ids();
        ASSERT_EQ(ids.size(), 1U);
        handlers.table().withSession(ids[0], [&](hipObj::v2::V2Session &s) {
            qp        = s.conn.qp.get();
            cq        = s.conn.cq.get();
            stagingMr = s.stagingMr.get();
            staging   = s.staging.get();
        });
        ASSERT_NE(qp, nullptr);
        ASSERT_NE(cq, nullptr);
        ASSERT_NE(stagingMr, nullptr);
        ASSERT_NE(staging, nullptr);
        /* No finishPrepareSend(): the session keeps its io reference */
    }
    EXPECT_EQ(g_fault.destroyQpCalls, 0);
    EXPECT_EQ(g_fault.destroyCqCalls, 0);
    EXPECT_EQ(g_fault.deregMrCalls, 0);
    freeAbandonedFakes(qp, cq, stagingMr, staging);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
