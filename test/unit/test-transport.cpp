/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * The RC transport: opening the device, creating and connecting the QP,
 * and polling for completions, both directly and through the public API,
 * against the fake device in fake-device.h.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#ifdef HIPOBJ_CPU_RDMA
#include "hip-shim.h"
#else
#include <hip/hip_runtime_api.h>
#endif
#include <sys/types.h>

#include "connection-pool.h"
#include "fake-device.h"
#include "hipobj-address.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "state.h"
#include "token.h"
#include "rc-listener.h"
#include "transport.h"
#include "v3-wire.h"
#include "vendor-ops.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::makeOps;
using hipObjTest::modifyQpCalls;
using hipObjTest::peerToken;
using hipObjTest::resetFake;
using hipObjTest::setReply;
#ifndef HIPOBJ_RDMA_WIRE_V3
using hipObjTest::setReplyWithPeerToken;
#endif /* !HIPOBJ_RDMA_WIRE_V3 */

void *const kDevBuf = hipObjTest::fakeDevBuf();

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

/* The modify_qp() masks for each transition */
constexpr int kRtrMask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
constexpr int kRtsMask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC;
constexpr int kInitMask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

#ifndef HIPOBJ_RDMA_WIRE_V3
/* The QPs a v1 instance creates with the default options: one for each
 * transfer, and the one hipObjGetRdmaToken() advertises */
constexpr int kDefaultQps = 1 + static_cast<int>(hipObj::Options::kDefaultMaxTransfers);
#endif

/* The default options, with maxTransfers connections for transfers. A
 * test passes this rather than declaring options of its own: CodeQL reads
 * a local in one of the fixtures' member functions as a global, and
 * reports locals with the same name as hiding it
 * (cpp/local-variable-hides-global-variable). */
#ifndef HIPOBJ_RDMA_WIRE_V3
hipObj::Options
withMaxTransfers(size_t maxTransfers)
{
    hipObj::Options result;
    result.maxTransfers = maxTransfers;
    return result;
}
#endif /* !HIPOBJ_RDMA_WIRE_V3 */

/* The states that the modify_qp() calls so far moved QPs to */
std::vector<ibv_qp_state>
modifiedStates()
{
    std::vector<ibv_qp_state> states;
    for (const hipObjTest::ModifyQpCall &call : modifyQpCalls()) {
        states.push_back(call.attr.qp_state);
    }
    return states;
}

class TransportTest : public hipObjTest::FakeDeviceTest {
protected:
    /* Opens the fake device in device_ */
    void openDevice()
    {
        std::optional<hipObj::RdmaDevice> device = hipObj::openRdmaDevice(verbs(), topology(), 0);
        ASSERT_TRUE(device);
        device_ = std::make_unique<hipObj::RdmaDevice>(std::move(*device));
    }

    /* Opens the fake device, and creates a connection on it in conn_.
     * Forgets the modify_qp() call that moved its QP to INIT, so a test
     * sees only the calls it makes. */
    void open()
    {
        ASSERT_NO_FATAL_FAILURE(openDevice());
        std::optional<hipObj::RcConnection> conn = hipObj::createRcConnection(*device_, 16, 8, 8);
        ASSERT_TRUE(conn);
        conn_                = std::make_unique<hipObj::RcConnection>(std::move(*conn));
        fake().modifyQpCount = 0;
    }

    /* A device on the fake context and protection domain, with gidIndex
     * as its GID index and no vendor ID */
    [[nodiscard]] hipObj::RdmaDevice deviceWithGidIndex(int gidIndex) const
    {
        return {verbs(),
                hipObj::own<hipObj::IbvContextPtr>(verbs(), verbs().open_device(nullptr)),
                hipObj::own<hipObj::IbvPdPtr>(verbs(), verbs().alloc_pd(nullptr)),
                1,
                gidIndex,
                {},
                std::nullopt};
    }

    /* Every verbs object the fakes created has been destroyed */
    static void expectNothingLeaked()
    {
        const hipObjTest::CallLog &log = fake().log;
        EXPECT_EQ(log.openDevice, log.closeDevice);
        EXPECT_EQ(log.allocPd, log.deallocPd);
        EXPECT_EQ(log.createCq, log.destroyCq);
        EXPECT_EQ(log.createQp, log.destroyQp);
        /* The RC-CM listener's, in v3 */
        EXPECT_EQ(log.createEventChannel, log.destroyEventChannel);
        EXPECT_EQ(log.createId, log.destroyId);
    }

    static void expectPeerGid(const ibv_qp_attr &attr)
    {
        const hipObj::RdmaToken token = peerToken();
        EXPECT_EQ(hipObj::toGid(attr.ah_attr.grh.dgid), token.gid);
    }

    /* conn_ is declared after device_, so it's destroyed first. They're
     * on the heap, which keeps the fixture small: CodeQL, which can't
     * expand TEST_F(), reads each test as a function that takes the
     * fixture by value, and reports a large one (cpp/large-parameter). */
    std::unique_ptr<hipObj::RdmaDevice>   device_{};
    std::unique_ptr<hipObj::RcConnection> conn_{};
};

// ---- Opening the device ------------------------------------------

TEST_F(TransportTest, OpenSetsUpTheDevice)
{
    const std::optional<hipObj::RdmaDevice> device = hipObj::openRdmaDevice(verbs(), topology(), 0);
    ASSERT_TRUE(device);
    EXPECT_EQ(&device->verbs(), &verbs());
    EXPECT_NE(device->context(), nullptr);
    EXPECT_NE(device->pd(), nullptr);
    EXPECT_EQ(device->portNum(), 1);
    /* The fake port has no GIDs, so the first one is used */
    EXPECT_EQ(device->gidIndex(), 0);
}

TEST_F(TransportTest, OpenRecordsTheVendorId)
{
    fake().vendorId                                = hipObj::kVendorIdPensando;
    const std::optional<hipObj::RdmaDevice> device = hipObj::openRdmaDevice(verbs(), topology(), 0);
    ASSERT_TRUE(device);
    EXPECT_EQ(device->vendorId(), hipObj::kVendorIdPensando);
    EXPECT_EQ(fake().log.queryDevice, 1);
}

TEST_F(TransportTest, OpenSucceedsWithoutVendorIdWhenDeviceQueryFails)
{
    fake().fail.queryDevice = true;
    {
        const std::optional<hipObj::RdmaDevice> device = hipObj::openRdmaDevice(verbs(), topology(), 0);
        ASSERT_TRUE(device);
        EXPECT_FALSE(device->vendorId());
    }
    expectNothingLeaked();
}

TEST_F(TransportTest, OpenRejectsIndexOutsideDeviceList)
{
    for (const int index : {-1, 1}) {
        EXPECT_FALSE(hipObj::openRdmaDevice(verbs(), topology(), index)) << "index " << index;
    }
    EXPECT_EQ(fake().log.openDevice, 0);
}

TEST_F(TransportTest, OpenFailsWithoutDeviceList)
{
    fake().fail.getDeviceList = true;
    EXPECT_FALSE(hipObj::openRdmaDevice(verbs(), topology(), 0));
    EXPECT_FALSE(hipObj::openRdmaDeviceByName(verbs(), topology(), "mlx5_0"));
}

TEST_F(TransportTest, OpenByNameFindsOnlyThatDevice)
{
    EXPECT_FALSE(hipObj::openRdmaDeviceByName(verbs(), topology(), ""));
    EXPECT_FALSE(hipObj::openRdmaDeviceByName(verbs(), topology(), "mlx5_1"));
    EXPECT_EQ(fake().log.openDevice, 0);

    EXPECT_TRUE(hipObj::openRdmaDeviceByName(verbs(), topology(), "mlx5_0"));
}

TEST_F(TransportTest, OpenReleasesWhatItAcquiredWhenAStepFails)
{
    for (bool hipObjTest::Faults::*const fault : {
             &hipObjTest::Faults::openDevice,
             &hipObjTest::Faults::allocPd,
             &hipObjTest::Faults::queryPort,
             &hipObjTest::Faults::queryGid,
         }) {
        resetFake();
        fake().fail.*fault = true;
        EXPECT_FALSE(hipObj::openRdmaDevice(verbs(), topology(), 0));
        expectNothingLeaked();
    }
}

// ---- Creating the connection -------------------------------------

TEST_F(TransportTest, CreateSetsUpTheConnection)
{
    ASSERT_NO_FATAL_FAILURE(openDevice());
    const std::optional<hipObj::RcConnection> conn = hipObj::createRcConnection(*device_, 16, 8, 8);
    ASSERT_TRUE(conn);
    EXPECT_EQ(&conn->device(), device_.get());
    EXPECT_NE(conn->cq(), nullptr);
    EXPECT_NE(conn->qp(), nullptr);
    EXPECT_EQ(fake().log.createCq, 1);
    EXPECT_EQ(fake().log.createQp, 1);
}

TEST_F(TransportTest, CreateReleasesCqWhenQpCreationFails)
{
    ASSERT_NO_FATAL_FAILURE(openDevice());
    fake().fail.createQp = true;
    EXPECT_FALSE(hipObj::createRcConnection(*device_, 16, 8, 8));
    EXPECT_EQ(fake().log.destroyCq, 1);

    fake().fail.createCq = true;
    EXPECT_FALSE(hipObj::createRcConnection(*device_, 16, 8, 8));
    device_.reset();
    expectNothingLeaked();
}

TEST_F(TransportTest, CreateReleasesQpAndCqWhenInitTransitionFails)
{
    ASSERT_NO_FATAL_FAILURE(openDevice());
    fake().fail.modifyQpToState = IBV_QPS_INIT;
    EXPECT_FALSE(hipObj::createRcConnection(*device_, 16, 8, 8));
    EXPECT_EQ(fake().modifyQpCount, 1U);
    EXPECT_EQ(fake().log.destroyQp, 1);
    EXPECT_EQ(fake().log.destroyCq, 1);
    device_.reset();
    expectNothingLeaked();
}

TEST_F(TransportTest, MovingKeepsTheQpAndDevice)
{
    ASSERT_NO_FATAL_FAILURE(open());
    ibv_qp *const                       qp = conn_->qp();
    std::optional<hipObj::RcConnection> moved;
    moved.emplace(std::move(*conn_));
    conn_.reset();
    EXPECT_EQ(fake().log.destroyQp, 0);
    EXPECT_EQ(moved->qp(), qp);
    EXPECT_EQ(&moved->device(), device_.get());
}

TEST_F(TransportTest, DestroyingReleasesEverything)
{
    ASSERT_NO_FATAL_FAILURE(open());
    EXPECT_EQ(&conn_->device(), device_.get());
    EXPECT_EQ(fake().log.destroyQp, 0);

    /* The connection doesn't own its device */
    conn_.reset();
    EXPECT_EQ(fake().log.destroyQp, 1);
    EXPECT_EQ(fake().log.destroyCq, 1);
    EXPECT_EQ(fake().log.deallocPd, 0);
    EXPECT_EQ(fake().log.closeDevice, 0);

    device_.reset();
    expectNothingLeaked();
}

TEST_F(TransportTest, CreateMovesQpToInitWithRemoteReadAndWrite)
{
    ASSERT_NO_FATAL_FAILURE(openDevice());
    ASSERT_TRUE(hipObj::createRcConnection(*device_, 16, 8, 8));

    std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_EQ(calls[0].attr.qp_state, IBV_QPS_INIT);
    EXPECT_EQ(calls[0].attr.port_num, device_->portNum());
    EXPECT_EQ(calls[0].attr.pkey_index, 0);
    EXPECT_EQ(calls[0].mask, kInitMask);
    /* The peer reads the buffer for a PUT and writes it for a GET, and
     * nothing else */
    EXPECT_EQ(calls[0].attr.qp_access_flags,
              static_cast<unsigned int>(IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
}

// ---- Connecting to the peer --------------------------------------

TEST_F(TransportTest, ConnectMovesQpToRtrThenRts)
{
    ASSERT_NO_FATAL_FAILURE(open());
    const hipObj::RdmaToken token = peerToken();
    ASSERT_TRUE(hipObj::connectRcPeer(*conn_, token));

    std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 2U);

    const ibv_qp_attr &rtr = calls[0].attr;
    EXPECT_EQ(calls[0].mask, kRtrMask);
    EXPECT_EQ(rtr.qp_state, IBV_QPS_RTR);
    EXPECT_EQ(rtr.path_mtu, IBV_MTU_4096);
    EXPECT_EQ(rtr.dest_qp_num, token.qpNum);
    EXPECT_EQ(rtr.rq_psn, 0U);
    EXPECT_EQ(rtr.max_dest_rd_atomic, 1);
    EXPECT_EQ(rtr.min_rnr_timer, 12);
    EXPECT_EQ(rtr.ah_attr.is_global, 1);
    EXPECT_EQ(rtr.ah_attr.dlid, token.lid);
    EXPECT_EQ(rtr.ah_attr.port_num, device_->portNum());
    EXPECT_EQ(rtr.ah_attr.grh.sgid_index, device_->gidIndex());
    EXPECT_EQ(rtr.ah_attr.grh.hop_limit, 64);
    expectPeerGid(rtr);

    const ibv_qp_attr &rts = calls[1].attr;
    EXPECT_EQ(calls[1].mask, kRtsMask);
    EXPECT_EQ(rts.qp_state, IBV_QPS_RTS);
    EXPECT_EQ(rts.timeout, 14);
    EXPECT_EQ(rts.retry_cnt, 7);
    EXPECT_EQ(rts.rnr_retry, 7);
    EXPECT_EQ(rts.sq_psn, 0U);
    EXPECT_EQ(rts.max_rd_atomic, 1);
}

/* An RC QP connects once: from RTS, it has to go back through RESET
 * before it can connect again, as the fake device enforces */
TEST_F(TransportTest, ConnectedQpConnectsAgainOnlyAfterReset)
{
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_TRUE(hipObj::connectRcPeer(*conn_, peerToken()));
    EXPECT_EQ(conn_->qp()->state, IBV_QPS_RTS);
    EXPECT_FALSE(hipObj::connectRcPeer(*conn_, peerToken()));

    ASSERT_TRUE(hipObj::resetRcConnection(*conn_));
    EXPECT_EQ(conn_->qp()->state, IBV_QPS_INIT);
    EXPECT_TRUE(hipObj::connectRcPeer(*conn_, peerToken()));
}

TEST_F(TransportTest, ResetMovesQpToInitThroughReset)
{
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_TRUE(hipObj::connectRcPeer(*conn_, peerToken()));
    fake().modifyQpCount = 0;
    ASSERT_TRUE(hipObj::resetRcConnection(*conn_));

    std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].attr.qp_state, IBV_QPS_RESET);
    EXPECT_EQ(calls[0].mask, IBV_QP_STATE);
    /* The same INIT transition as creating the connection, which doesn't
     * widen the access flags */
    EXPECT_EQ(calls[1].attr.qp_state, IBV_QPS_INIT);
    EXPECT_EQ(calls[1].mask, kInitMask);
    EXPECT_EQ(calls[1].attr.qp_access_flags,
              static_cast<unsigned int>(IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
}

TEST_F(TransportTest, ResetDrainsTheCq)
{
    ASSERT_NO_FATAL_FAILURE(open());

    /* Polls until the CQ is empty */
    fake().pollResult = 0;
    ASSERT_TRUE(hipObj::resetRcConnection(*conn_));
    EXPECT_EQ(fake().log.pollCq, 1);

    /* A CQ that always has a completion is polled no more times than it
     * can hold completions */
    fake().log.pollCq = 0;
    fake().pollResult = 1;
    ASSERT_TRUE(hipObj::resetRcConnection(*conn_));
    EXPECT_EQ(fake().log.pollCq, conn_->cq()->cqe);
}

TEST_F(TransportTest, ResetFailsWhenAStepFails)
{
    ASSERT_NO_FATAL_FAILURE(open());
    fake().fail.modifyQpToState = IBV_QPS_RESET;
    EXPECT_FALSE(hipObj::resetRcConnection(*conn_));
    EXPECT_EQ(fake().log.pollCq, 0);

    fake().fail.modifyQpToState = -1;
    fake().pollResult           = -1;
    fake().modifyQpCount        = 0;
    EXPECT_FALSE(hipObj::resetRcConnection(*conn_));
    EXPECT_EQ(modifiedStates(), std::vector<ibv_qp_state>({IBV_QPS_RESET}));

    fake().pollResult           = 0;
    fake().fail.modifyQpToState = IBV_QPS_INIT;
    EXPECT_FALSE(hipObj::resetRcConnection(*conn_));
}

TEST_F(TransportTest, ConnectDoesNotQueryTheDevice)
{
    /* The vendor ID was read when the device opened */
    fake().vendorId = hipObj::kVendorIdBroadcom;
    ASSERT_NO_FATAL_FAILURE(open());
    const int queries = fake().log.queryDevice;
    ASSERT_TRUE(hipObj::connectRcPeer(*conn_, peerToken()));
    EXPECT_EQ(fake().log.queryDevice, queries);
}

TEST_F(TransportTest, ConnectRejectsPeerThatIsNotRc)
{
    ASSERT_NO_FATAL_FAILURE(open());
    EXPECT_FALSE(hipObj::connectRcPeer(*conn_, peerToken(hipObj::TransportType::DC)));
    EXPECT_EQ(fake().modifyQpCount, 0U);
}

TEST_F(TransportTest, ConnectStopsAtFailedTransition)
{
    ASSERT_NO_FATAL_FAILURE(open());
    fake().fail.modifyQpToState = IBV_QPS_RTR;
    EXPECT_FALSE(hipObj::connectRcPeer(*conn_, peerToken()));
    EXPECT_EQ(fake().modifyQpCount, 1U);

    fake().modifyQpCount        = 0;
    fake().fail.modifyQpToState = IBV_QPS_RTS;
    EXPECT_FALSE(hipObj::connectRcPeer(*conn_, peerToken()));
    EXPECT_EQ(fake().modifyQpCount, 2U);
}

TEST_F(TransportTest, RtrRejectsGidIndexTheAddressHandleCannotHold)
{
    /* The address handle holds the GID index in a uint8_t, so anything
     * outside [0, 255] must be refused, not truncated */
    const ibv_gid gid{};
    for (const int index : {-1, 256, 0x10000}) {
        const hipObj::RdmaDevice            device = deviceWithGidIndex(index);
        std::optional<hipObj::RcConnection> conn   = hipObj::createRcConnection(device, 16, 8, 8);
        ASSERT_TRUE(conn);
        /* Only the RTR transition's calls count */
        fake().modifyQpCount = 0;
        EXPECT_FALSE(hipObj::transitionQpToRtr(*conn, 1, 0, gid)) << "index " << index;
        EXPECT_EQ(fake().modifyQpCount, 0U) << "index " << index;
    }

    const hipObj::RdmaDevice            device = deviceWithGidIndex(255);
    std::optional<hipObj::RcConnection> conn   = hipObj::createRcConnection(device, 16, 8, 8);
    ASSERT_TRUE(conn);
    fake().modifyQpCount = 0;
    ASSERT_TRUE(hipObj::transitionQpToRtr(*conn, 1, 0, gid));
    EXPECT_EQ(modifyQpCalls().at(0).attr.ah_attr.grh.sgid_index, 255);
}

// ---- Vendor QP attributes ----------------------------------------

/* The vendor backends set their attributes before the generic ones, so the
 * RTR transition, which doesn't set the timeout or retries itself, shows
 * whether they ran */
struct VendorCase {
    const char *name;
    uint32_t    vendorId;
    bool        configured;
};

class VendorQpTest : public TransportTest, public ::testing::WithParamInterface<VendorCase> {};

TEST_P(VendorQpTest, RtrHasVendorAttributes)
{
    /* The device reads its vendor ID when it opens */
    fake().vendorId = GetParam().vendorId;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_TRUE(hipObj::connectRcPeer(*conn_, peerToken()));

    const ibv_qp_attr rtr = modifyQpCalls().at(0).attr;
    if (GetParam().configured) {
        EXPECT_EQ(rtr.path_mtu, IBV_MTU_4096);
        EXPECT_EQ(rtr.timeout, 14);
        EXPECT_EQ(rtr.retry_cnt, 7);
        EXPECT_EQ(rtr.rnr_retry, 7);
    }
    else {
        EXPECT_EQ(rtr.timeout, 0);
        EXPECT_EQ(rtr.retry_cnt, 0);
        EXPECT_EQ(rtr.rnr_retry, 0);
    }
}

/* Only the backends that were built configure the QP */
constexpr auto kVendorCases = std::to_array<VendorCase>({
    {.name = "Unknown", .vendorId = 0x15b3, .configured = false},
#ifdef HIPOBJ_BNXT
    {.name = "Broadcom", .vendorId = hipObj::kVendorIdBroadcom, .configured = true},
#endif
#ifdef HIPOBJ_IONIC
    {.name = "Pensando", .vendorId = hipObj::kVendorIdPensando, .configured = true},
#endif
    {.name = "None", .vendorId = 0, .configured = false},
});

INSTANTIATE_TEST_SUITE_P(Transport, VendorQpTest, ::testing::ValuesIn(kVendorCases),
                         [](const ::testing::TestParamInfo<VendorCase> &paramInfo) {
                             return paramInfo.param.name;
                         });

TEST_F(TransportTest, NoVendorAttributesWhenDeviceQueryFails)
{
    /* The query fails when the device opens */
    fake().vendorId         = hipObj::kVendorIdBroadcom;
    fake().fail.queryDevice = true;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_TRUE(hipObj::connectRcPeer(*conn_, peerToken()));
    EXPECT_EQ(modifyQpCalls().at(0).attr.timeout, 0);
}

// ---- Polling for the completion ----------------------------------

TEST_F(TransportTest, PollReturnsOnCompletion)
{
    ASSERT_NO_FATAL_FAILURE(open());
    EXPECT_TRUE(hipObj::pollCompletion(*conn_, std::nullopt, std::chrono::seconds(1)));
    fake().pollOpcode = IBV_WC_RDMA_READ;
    EXPECT_TRUE(hipObj::pollCompletion(*conn_, IBV_WC_RDMA_READ, std::chrono::seconds(1)));
}

TEST_F(TransportTest, PollFailsOnErrorCompletion)
{
    ASSERT_NO_FATAL_FAILURE(open());
    fake().pollStatus = IBV_WC_REM_ACCESS_ERR;
    EXPECT_FALSE(hipObj::pollCompletion(*conn_, std::nullopt, std::chrono::seconds(1)));
}

TEST_F(TransportTest, PollFailsWhenPollingFails)
{
    ASSERT_NO_FATAL_FAILURE(open());
    fake().pollResult = -1;
    EXPECT_FALSE(hipObj::pollCompletion(*conn_, std::nullopt, std::chrono::seconds(1)));
}

TEST_F(TransportTest, PollTimesOutWithoutTheAwaitedCompletion)
{
    ASSERT_NO_FATAL_FAILURE(open());
    /* Nothing completes */
    fake().pollResult = 0;
    EXPECT_FALSE(hipObj::pollCompletion(*conn_, std::nullopt, std::chrono::milliseconds(10)));
    /* Only other work completes */
    fake().pollResult = 1;
    fake().pollOpcode = IBV_WC_SEND;
    EXPECT_FALSE(hipObj::pollCompletion(*conn_, IBV_WC_RDMA_READ, std::chrono::milliseconds(10)));
}

// ---- Initialization ----------------------------------------------

TEST_F(TransportTest, InitFailsAndReleasesEverythingWhenSetupFails)
{
    for (bool hipObjTest::Faults::*const fault : {
             &hipObjTest::Faults::getDeviceList,
             &hipObjTest::Faults::openDevice,
             &hipObjTest::Faults::allocPd,
             &hipObjTest::Faults::queryPort,
             &hipObjTest::Faults::queryGid,
#ifndef HIPOBJ_RDMA_WIRE_V3
             /* Only v1's hipObjInit() creates a CQ and a QP */
             &hipObjTest::Faults::createCq,
             &hipObjTest::Faults::createQp,
#endif
         }) {
        resetFake();
        fake().fail.*fault          = true;
        const hipObjConfig_t config = makeConfig();
        EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
        EXPECT_EQ(hipObj::currentInstance(), nullptr);
        expectNothingLeaked();
    }
}

#ifndef HIPOBJ_RDMA_WIRE_V3
TEST_F(TransportTest, InitFailsAndReleasesEverythingWhenInitTransitionFails)
{
    fake().fail.modifyQpToState = IBV_QPS_INIT;
    const hipObjConfig_t config = makeConfig();
    EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    expectNothingLeaked();
}

TEST_F(TransportTest, InitFailsAndReleasesEverythingWhenThePoolCantBeCreated)
{
    /* The token connection, and one of the pool's three */
    fake().fail.qpLimit = 2;
    EXPECT_EQ(initFakes(makeConfig(), withMaxTransfers(3)).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    EXPECT_EQ(fake().log.createQp, 2);
    expectNothingLeaked();
}
#endif /* !HIPOBJ_RDMA_WIRE_V3 */

TEST_F(TransportTest, InitFailsForUnknownNic)
{
    hipObjConfig_t config = makeConfig();
    config.nicHint        = "mlx5_1";
    EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
}

TEST_F(TransportTest, ShutdownReleasesEverything)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
#ifndef HIPOBJ_RDMA_WIRE_V3
    EXPECT_EQ(fake().log.createQp, kDefaultQps);
#endif
    expectNothingLeaked();
}

// ---- Transfers ---------------------------------------------------

using TransferFn = hipObjError_t (*)(hipObjOps_t *);

hipObjError_t
callGet(hipObjOps_t *ops)
{
    return hipObjGet(nullptr, kDevBuf, kBufSize, 0, ops, nullptr);
}

hipObjError_t
callPut(hipObjOps_t *ops)
{
    return hipObjPut(nullptr, kDevBuf, kBufSize, 0, ops, nullptr);
}

int
failSendRequest(void * /*ctx*/, const char * /*token*/, size_t /*tokenLen*/)
{
    return -1;
}

int
failRecvReply(void * /*ctx*/, char * /*reply*/, size_t * /*replyLen*/)
{
    return 5;
}

#ifndef HIPOBJ_RDMA_WIRE_V3
/* What recordSendRequest() was passed: the token's length, and its bytes
 * up to and including the one after it */
struct SentToken {
    size_t               len = 0;
    hipObj::RdmaTokenHex bytes{};
};

int
recordSendRequest(void *ctx, const char *token, size_t tokenLen)
{
    auto *sent = static_cast<SentToken *>(ctx);
    sent->len  = tokenLen;
    if (tokenLen < sent->bytes.size()) {
        std::ranges::copy(hipObj::spanFromPointer(token, tokenLen + 1), sent->bytes.begin());
    }
    return 0;
}
#endif /* !HIPOBJ_RDMA_WIRE_V3 */

class TransportTransferTest : public TransportTest, public ::testing::WithParamInterface<TransferFn> {
protected:
    static hipObjError_t transfer()
    {
        hipObjOps_t ops = makeOps();
        return GetParam()(&ops);
    }

    /* Initializes with options, and registers kBufSize bytes at kDevBuf.
     * Forgets the modify_qp() calls that moved the QPs to INIT, so a test
     * sees only the transfers' calls. */
    void initForTransfers(const hipObj::Options &options = {}) const
    {
        ASSERT_EQ(initFakes(makeConfig(), options).opError, hipObjSuccess);
        ASSERT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjSuccess);
        fake().modifyQpCount = 0;
    }

#ifndef HIPOBJ_RDMA_WIRE_V3
    /* initForTransfers() with one connection for transfers, so each one
     * uses it */
    void initForTransfersOnOneConnection() const
    {
        ASSERT_NO_FATAL_FAILURE(initForTransfers(withMaxTransfers(1)));
    }
#endif /* !HIPOBJ_RDMA_WIRE_V3 */
};

#ifndef HIPOBJ_RDMA_WIRE_V3
TEST_P(TransportTransferTest, ConnectsToPeerInReply)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfers());
    setReplyWithPeerToken(peerToken());
    const int queries = fake().log.queryDevice;
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    /* Connecting reads the vendor ID initialization stored, not the device */
    EXPECT_EQ(fake().log.queryDevice, queries);

    /* The reply's token connects the QP, and it's reset to INIT once the
     * transfer is over */
    std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 4U);
    EXPECT_EQ(calls[0].attr.qp_state, IBV_QPS_RTR);
    EXPECT_EQ(calls[0].attr.dest_qp_num, peerToken().qpNum);
    expectPeerGid(calls[0].attr);
    EXPECT_EQ(calls[1].attr.qp_state, IBV_QPS_RTS);
    EXPECT_EQ(calls[2].attr.qp_state, IBV_QPS_RESET);
    EXPECT_EQ(calls[3].attr.qp_state, IBV_QPS_INIT);
}

/* docs/interop.rst reserves the peer token's rkey, address, length, and
 * port number: servers send zero, and clients ignore them. The QP connects
 * the same way whatever they hold, and on the local port, not the peer's. */
TEST_P(TransportTransferTest, IgnoresReservedFieldsOfPeerToken)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfersOnOneConnection());
    const uint8_t localPort = hipObj::currentInstance()->device->portNum();

    hipObj::RdmaToken zeroed = peerToken();
    zeroed.rkey              = 0;
    zeroed.remoteAddr        = 0;
    zeroed.length            = 0;
    zeroed.portNum           = 0;

    hipObj::RdmaToken nonzero = peerToken();
    nonzero.rkey              = 0xfeedfaceU;
    nonzero.remoteAddr        = 0xdead0000U;
    nonzero.length            = kBufSize * 3;
    nonzero.portNum           = static_cast<uint8_t>(localPort + 7);

    for (const hipObj::RdmaToken &token : {zeroed, nonzero}) {
        fake().modifyQpCount = 0;
        setReplyWithPeerToken(token);
        ASSERT_EQ(transfer().opError, hipObjSuccess);

        const std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
        ASSERT_EQ(calls.size(), 4U);
        const ibv_qp_attr &rtr = calls[0].attr;
        EXPECT_EQ(rtr.qp_state, IBV_QPS_RTR);
        EXPECT_EQ(calls[0].mask, kRtrMask);
        EXPECT_EQ(rtr.dest_qp_num, token.qpNum);
        EXPECT_EQ(rtr.ah_attr.dlid, token.lid);
        EXPECT_EQ(rtr.ah_attr.port_num, localPort);
        expectPeerGid(rtr);
        EXPECT_EQ(calls[1].attr.qp_state, IBV_QPS_RTS);
        EXPECT_EQ(calls[1].mask, kRtsMask);
    }
}

/* A QP that's connected can't connect again until it's reset, so this
 * fails if the transfer doesn't reset it */
TEST_P(TransportTransferTest, PeerTokenTransfersBackToBack)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfersOnOneConnection());
    setReplyWithPeerToken(peerToken());
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    EXPECT_EQ(modifiedStates(),
              std::vector<ibv_qp_state>({IBV_QPS_RTR, IBV_QPS_RTS, IBV_QPS_RESET, IBV_QPS_INIT, IBV_QPS_RTR,
                                         IBV_QPS_RTS, IBV_QPS_RESET, IBV_QPS_INIT}));
}

TEST_P(TransportTransferTest, DoesNotConnectWithoutPeerToken)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfers());
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    EXPECT_EQ(fake().modifyQpCount, 0U);
}

TEST_P(TransportTransferTest, RejectsPeerTokenThatIsNotRc)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfers());
    setReplyWithPeerToken(peerToken(hipObj::TransportType::DC));
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
    /* The QP isn't connected, though it's still reset */
    EXPECT_EQ(modifiedStates(), std::vector<ibv_qp_state>({IBV_QPS_RESET, IBV_QPS_INIT}));
}

TEST_P(TransportTransferTest, FailsWhenConnectionFails)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfersOnOneConnection());
    setReplyWithPeerToken(peerToken());
    fake().fail.modifyQpToState = IBV_QPS_RTS;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);

    /* The QP got as far as RTR, and it's reset, so the next transfer can
     * connect it */
    fake().fail.modifyQpToState = -1;
    EXPECT_EQ(transfer().opError, hipObjSuccess);
    EXPECT_EQ(modifiedStates(),
              std::vector<ibv_qp_state>({IBV_QPS_RTR, IBV_QPS_RTS, IBV_QPS_RESET, IBV_QPS_INIT, IBV_QPS_RTR,
                                         IBV_QPS_RTS, IBV_QPS_RESET, IBV_QPS_INIT}));
}

/* A connection that can't be reset is replaced */
TEST_P(TransportTransferTest, ReplacesAConnectionItCantReset)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfersOnOneConnection());
    const int qps = fake().log.createQp;
    setReplyWithPeerToken(peerToken());
    fake().fail.modifyQpToState = IBV_QPS_RESET;
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    EXPECT_EQ(fake().log.createQp, qps + 1);
    EXPECT_EQ(fake().log.destroyQp, 1);

    fake().fail.modifyQpToState = -1;
    EXPECT_EQ(transfer().opError, hipObjSuccess);
}

/* A connection that can't be reset or replaced is dropped, and with none
 * left, transfers fail */
TEST_P(TransportTransferTest, FailsWithoutConnections)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfersOnOneConnection());
    setReplyWithPeerToken(peerToken());
    fake().fail.modifyQpToState = IBV_QPS_RESET;
    fake().fail.createQp        = true;
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    EXPECT_EQ(hipObj::currentInstance()->pool.size(), 0U);

    const int requests = fake().log.sendRequest;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.sendRequest, requests);
}

TEST_P(TransportTransferTest, FailsOnErrorCompletion)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfers());
    fake().pollStatus = IBV_WC_REM_ACCESS_ERR;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
}
#endif /* !HIPOBJ_RDMA_WIRE_V3 */

TEST_P(TransportTransferTest, FailsWhenDeviceSyncFails)
{
    /* A GPU-direct buffer is synchronized after the transfer */
    ASSERT_NO_FATAL_FAILURE(initForTransfers());
    fake().deviceSyncErr = hipErrorLaunchFailure;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
}

TEST_P(TransportTransferTest, ReportsS3Failures)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfers());

    hipObjOps_t ops = makeOps();
    ops.sendRequest = &failSendRequest;
    EXPECT_EQ(GetParam()(&ops).opError, hipObjS3Error);

    ops           = makeOps();
    ops.recvReply = &failRecvReply;
    EXPECT_EQ(GetParam()(&ops).opError, hipObjS3Error);

    /* The server reports an error, or a reply that can't be parsed */
    for (const char *reply : {"500", "err", "20x", ""}) {
        setReply(reply);
        EXPECT_EQ(transfer().opError, hipObjS3Error) << "reply \"" << reply << "\"";
    }
    EXPECT_EQ(fake().modifyQpCount, 0U);
}

#ifdef HIPOBJ_RDMA_WIRE_V3
/* A transfer succeeds only when the server moved the whole range, since
 * hipObjGet() and hipObjPut() can't report that fewer bytes moved */
TEST_P(TransportTransferTest, SucceedsOnlyWhenTheWholeRangeMoved)
{
    ASSERT_NO_FATAL_FAILURE(initForTransfers());
    for (const uint64_t bytes : {
             uint64_t{0},
             uint64_t{kBufSize - 1},
             uint64_t{kBufSize + 1},
             std::numeric_limits<uint64_t>::max(),
         }) {
        setReply(hipObj::v3::encodeRdmaReplyV3({.httpStatus = 200, .bytesTransferred = bytes}));
        EXPECT_EQ(transfer().opError, hipObjS3Error) << bytes << " bytes";
    }
    setReply(hipObj::v3::encodeRdmaReplyV3({.httpStatus = 200, .bytesTransferred = kBufSize}));
    EXPECT_EQ(transfer().opError, hipObjSuccess);
}

/* Records the v3 token a transfer sends, decoded, in the
 * std::optional<RdmaTokenV3> ctx points to */
int
recordV3SendRequest(void *ctx, const char *token, size_t tokenLen)
{
    hipObjTest::noteSentToken(token, tokenLen);
    auto *sent = static_cast<std::optional<hipObj::v3::RdmaTokenV3> *>(ctx);
    *sent = hipObj::v3::decodeRdmaTokenV3(hipObj::asStringView(hipObj::spanFromPointer(token, tokenLen)));
    return 0;
}

/* The listener binds to the chosen NIC's address, and the token
 * advertises that address and the listener's port */
TEST_F(TransportTest, TokenAdvertisesTheListener)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    EXPECT_EQ(rdmaCm().boundAddress(), "192.0.2.1");
    hipObjOps_t ops = makeOps();
    ops.sendRequest = &recordV3SendRequest;
    std::optional<hipObj::v3::RdmaTokenV3> sent;
    ASSERT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, &sent).opError, hipObjSuccess);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent->listenIp, "192.0.2.1");
    EXPECT_EQ(sent->listenPort, hipObj::kRcListenerDefaultPort);
    EXPECT_EQ(sent->length, kBufSize);
    EXPECT_EQ(fake().log.listen, 1);
}

/* HIPOBJ_RC_LISTEN_IP names the address to listen on and advertise, and
 * the NIC is the one its bind resolves to, whatever the hint names.
 * HIPOBJ_RC_LISTEN_PORT names the port. */
TEST_F(TransportTest, ListenIpPicksTheNic)
{
    hipObj::Options options;
    options.rcListenIp    = "192.0.2.1";
    options.rcListenPort  = 23456;
    hipObjConfig_t config = makeConfig();
    config.nicHint        = "mlx5_9";
    ASSERT_EQ(initFakes(config, options).opError, hipObjSuccess);
    ASSERT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(rdmaCm().boundPort(), "23456");

    hipObjOps_t ops = makeOps();
    ops.sendRequest = &recordV3SendRequest;
    std::optional<hipObj::v3::RdmaTokenV3> sent;
    ASSERT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, &sent).opError, hipObjSuccess);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent->listenIp, "192.0.2.1");
    EXPECT_EQ(sent->listenPort, 23456);
}

/* The server dials the listener, so without one no transfer could work:
 * hipObjInit() fails, and releases what it acquired, when the NIC has no
 * IPv4 address to bind to, the listener doesn't start, its bind resolves
 * to another NIC, or HIPOBJ_RC_LISTEN_IP is no NIC's address */
TEST_F(TransportTest, InitFailsWhenTheListenerCannotStart)
{
    const hipObjConfig_t config = makeConfig();

    fake().gid = {};
    EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.createEventChannel, 0);

    fake().gid           = hipObjTest::kFakeNicGid;
    fake().fail.bindAddr = true;
    EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.bindAddr, 1);

    fake().fail.bindAddr  = false;
    fake().listenerDevice = "mlx5_1";
    EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.listen, 1);

    /* Before any device is opened */
    fake().listenerDevice = "mlx5_0";
    hipObj::Options options;
    options.rcListenIp = "192.0.2.99";
    EXPECT_EQ(initFakes(config, options).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.openDevice, 3);

    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    expectNothingLeaked();
}
#endif /* HIPOBJ_RDMA_WIRE_V3 */

#ifndef HIPOBJ_RDMA_WIRE_V3
/* Only v1 creates connections of its own */
TEST_F(TransportTest, InitCreatesAConnectionForEachTransferAndOneForTokens)
{
    ASSERT_EQ(initFakes(makeConfig(), withMaxTransfers(3)).opError, hipObjSuccess);
    ASSERT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.createQp, 4);
    EXPECT_EQ(fake().log.createCq, 4);
    const std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 4U);
    for (const hipObjTest::ModifyQpCall &call : calls) {
        EXPECT_EQ(call.attr.qp_state, IBV_QPS_INIT);
    }

    /* A transfer's token names a connection from the pool, not the one
     * hipObjGetRdmaToken() advertises */
    hipObjOps_t ops = makeOps();
    ops.sendRequest = &recordSendRequest;
    SentToken sent;
    ASSERT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, &sent).opError, hipObjSuccess);
    const auto transferToken =
        hipObj::decodeRdmaTokenHex(hipObj::asStringView(std::span(sent.bytes).first(sent.len)));
    ASSERT_TRUE(transferToken);

    char *token = nullptr;
    ASSERT_EQ(hipObjGetRdmaToken(kDevBuf, kBufSize, HIPOBJ_RDMA_OP_GET, &token).opError, hipObjSuccess);
    const auto appToken =
        hipObj::decodeRdmaTokenHex(hipObj::cStringView(token, hipObj::kRdmaTokenHexLen + 1));
    EXPECT_EQ(hipObjPutRdmaToken(token).opError, hipObjSuccess);
    ASSERT_TRUE(appToken);
    EXPECT_NE(transferToken->qpNum, appToken->qpNum);
}

TEST_F(TransportTest, SendsTokenForTheRegisteredRange)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    constexpr off_t  kOffset = 16;
    constexpr size_t kSize   = kBufSize - 16;

    hipObjOps_t ops = makeOps();
    ops.sendRequest = &recordSendRequest;
    for (const bool isGet : {true, false}) {
        SentToken           sent;
        const hipObjError_t err = isGet ? hipObjGet(nullptr, kDevBuf, kSize, kOffset, &ops, &sent)
                                        : hipObjPut(nullptr, kDevBuf, kSize, kOffset, &ops, &sent);
        ASSERT_EQ(err.opError, hipObjSuccess) << (isGet ? "get" : "put");

        /* Callbacks that treat the token as a C string still work */
        ASSERT_EQ(sent.len, hipObj::kRdmaTokenHexLen);
        EXPECT_EQ(sent.bytes[hipObj::kRdmaTokenHexLen], '\0');

        const auto token =
            hipObj::decodeRdmaTokenHex(hipObj::asStringView(std::span(sent.bytes).first(sent.len)));
        ASSERT_TRUE(token);
        EXPECT_EQ(token->transport, hipObj::TransportType::RC);
        EXPECT_EQ(token->rkey, 0x1234U);
        EXPECT_EQ(token->remoteAddr, hipObj::pointerToAddress(kDevBuf) + kOffset);
        EXPECT_EQ(token->length, kSize);
    }
}
#endif /* !HIPOBJ_RDMA_WIRE_V3 */

INSTANTIATE_TEST_SUITE_P(Transport, TransportTransferTest, ::testing::Values(&callGet, &callPut),
                         [](const ::testing::TestParamInfo<TransferFn> &paramInfo) {
                             return paramInfo.param == &callGet ? "Get" : "Put";
                         });

} // namespace
