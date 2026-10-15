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
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>
#include <sys/types.h>

#include "fake-device.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "state.h"
#include "token.h"
#include "transport.h"
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
using hipObjTest::setReplyWithPeerToken;

void *const kDevBuf = hipObjTest::fakeDevBuf();

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

/* The modify_qp() masks for each transition */
constexpr int kRtrMask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
constexpr int kRtsMask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC;

class TransportTest : public hipObjTest::FakeDeviceTest {
protected:
    /* Opens the fake device in device_ */
    void openDevice()
    {
        std::optional<hipObj::RdmaDevice> device = hipObj::openRdmaDevice(verbs(), topology(), 0);
        ASSERT_TRUE(device);
        device_.emplace(std::move(*device));
    }

    /* Opens the fake device, and creates a connection on it in conn_.
     * Forgets the modify_qp() call that moved its QP to INIT, so a test
     * sees only the calls it makes. */
    void open()
    {
        ASSERT_NO_FATAL_FAILURE(openDevice());
        std::optional<hipObj::RcConnection> conn = hipObj::createRcConnection(*device_, 16, 8, 8);
        ASSERT_TRUE(conn);
        conn_.emplace(std::move(*conn));
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
    }

    static void expectPeerGid(const struct ibv_qp_attr &attr)
    {
        const hipObj::RdmaToken token = peerToken();
        EXPECT_EQ(hipObj::toGid(attr.ah_attr.grh.dgid), token.gid);
    }

    /* conn_ is declared after device_, so it's destroyed first */
    std::optional<hipObj::RdmaDevice>   device_{};
    std::optional<hipObj::RcConnection> conn_{};
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
    for (int index : {-1, 1}) {
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
    for (bool hipObjTest::Faults::*fault : {&hipObjTest::Faults::openDevice, &hipObjTest::Faults::allocPd,
                                            &hipObjTest::Faults::queryPort, &hipObjTest::Faults::queryGid}) {
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
    EXPECT_EQ(&conn->device(), &*device_);
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
    struct ibv_qp *const                qp = conn_->qp();
    std::optional<hipObj::RcConnection> moved;
    moved.emplace(std::move(*conn_));
    conn_.reset();
    EXPECT_EQ(fake().log.destroyQp, 0);
    EXPECT_EQ(moved->qp(), qp);
    EXPECT_EQ(&moved->device(), &*device_);
}

TEST_F(TransportTest, DestroyingReleasesEverything)
{
    ASSERT_NO_FATAL_FAILURE(open());
    EXPECT_EQ(&conn_->device(), &*device_);
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
    EXPECT_EQ(calls[0].mask, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
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

    const struct ibv_qp_attr &rtr = calls[0].attr;
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

    const struct ibv_qp_attr &rts = calls[1].attr;
    EXPECT_EQ(calls[1].mask, kRtsMask);
    EXPECT_EQ(rts.qp_state, IBV_QPS_RTS);
    EXPECT_EQ(rts.timeout, 14);
    EXPECT_EQ(rts.retry_cnt, 7);
    EXPECT_EQ(rts.rnr_retry, 7);
    EXPECT_EQ(rts.sq_psn, 0U);
    EXPECT_EQ(rts.max_rd_atomic, 1);
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
    ibv_gid gid{};
    for (int index : {-1, 256, 0x10000}) {
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

    const struct ibv_qp_attr rtr = modifyQpCalls().at(0).attr;
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
    {"Unknown", 0x15b3, false},
#ifdef HIPOBJ_BNXT
    {"Broadcom", hipObj::kVendorIdBroadcom, true},
#endif
#ifdef HIPOBJ_IONIC
    {"Pensando", hipObj::kVendorIdPensando, true},
#endif
    {"None", 0, false},
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
    for (bool hipObjTest::Faults::*fault :
         {&hipObjTest::Faults::getDeviceList, &hipObjTest::Faults::openDevice, &hipObjTest::Faults::allocPd,
          &hipObjTest::Faults::queryPort, &hipObjTest::Faults::queryGid, &hipObjTest::Faults::createCq,
          &hipObjTest::Faults::createQp}) {
        resetFake();
        fake().fail.*fault    = true;
        hipObjConfig_t config = makeConfig();
        EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
        EXPECT_EQ(hipObj::currentInstance(), nullptr);
        expectNothingLeaked();
    }
}

TEST_F(TransportTest, InitFailsAndReleasesEverythingWhenInitTransitionFails)
{
    fake().fail.modifyQpToState = IBV_QPS_INIT;
    hipObjConfig_t config       = makeConfig();
    EXPECT_EQ(initFakes(config).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    expectNothingLeaked();
}

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
    EXPECT_EQ(fake().log.createQp, 1);
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
failSendRequest(void *, const char *, size_t)
{
    return -1;
}

int
failRecvReply(void *, char *, size_t *)
{
    return 5;
}

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

class TransportTransferTest : public TransportTest, public ::testing::WithParamInterface<TransferFn> {
protected:
    hipObjError_t transfer()
    {
        hipObjOps_t ops = makeOps();
        return GetParam()(&ops);
    }
};

TEST_P(TransportTransferTest, ConnectsToPeerInReply)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    setReplyWithPeerToken(peerToken());
    const int queries = fake().log.queryDevice;
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    /* Connecting reads the vendor ID initialization stored, not the device */
    EXPECT_EQ(fake().log.queryDevice, queries);

    /* Initialization moved the QP to INIT; the reply's token connects it */
    std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 3U);
    EXPECT_EQ(calls[0].attr.qp_state, IBV_QPS_INIT);
    EXPECT_EQ(calls[1].attr.qp_state, IBV_QPS_RTR);
    EXPECT_EQ(calls[1].attr.dest_qp_num, peerToken().qpNum);
    expectPeerGid(calls[1].attr);
    EXPECT_EQ(calls[2].attr.qp_state, IBV_QPS_RTS);
}

TEST_P(TransportTransferTest, DoesNotConnectWithoutPeerToken)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    ASSERT_EQ(transfer().opError, hipObjSuccess);
    EXPECT_EQ(fake().modifyQpCount, 1U);
}

TEST_P(TransportTransferTest, RejectsPeerTokenThatIsNotRc)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    setReplyWithPeerToken(peerToken(hipObj::TransportType::DC));
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
    EXPECT_EQ(fake().modifyQpCount, 1U);
}

TEST_P(TransportTransferTest, FailsWhenConnectionFails)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    setReplyWithPeerToken(peerToken());
    fake().fail.modifyQpToState = IBV_QPS_RTR;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
}

TEST_P(TransportTransferTest, FailsOnErrorCompletion)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    fake().pollStatus = IBV_WC_REM_ACCESS_ERR;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
}

TEST_P(TransportTransferTest, FailsWhenDeviceSyncFails)
{
    /* A GPU-direct buffer is synchronized after the transfer */
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    fake().deviceSyncErr = hipErrorLaunchFailure;
    EXPECT_EQ(transfer().opError, hipObjRdmaError);
}

TEST_P(TransportTransferTest, ReportsS3Failures)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());

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
    EXPECT_EQ(fake().modifyQpCount, 1U);
}

TEST_F(TransportTest, SendsTokenForTheRegisteredRange)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    constexpr off_t  kOffset = 16;
    constexpr size_t kSize   = kBufSize - 16;

    hipObjOps_t ops = makeOps();
    ops.sendRequest = &recordSendRequest;
    for (bool isGet : {true, false}) {
        SentToken     sent;
        hipObjError_t err = isGet ? hipObjGet(nullptr, kDevBuf, kSize, kOffset, &ops, &sent)
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
        EXPECT_EQ(token->remoteAddr, reinterpret_cast<uint64_t>(kDevBuf) + kOffset);
        EXPECT_EQ(token->length, kSize);
    }
}

INSTANTIATE_TEST_SUITE_P(Transport, TransportTransferTest, ::testing::Values(&callGet, &callPut),
                         [](const ::testing::TestParamInfo<TransferFn> &paramInfo) {
                             return paramInfo.param == &callGet ? "Get" : "Put";
                         });

} // namespace
