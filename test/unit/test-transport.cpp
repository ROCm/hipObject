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
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#ifdef HIPOBJ_CPU_RDMA
#include "hip-shim.h"
#else
#include <hip/hip_runtime_api.h>
#endif
#include <sys/types.h>

#include "fake-device.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "state.h"
#include "token.h"
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
#ifdef HIPOBJ_RDMA_WIRE_V1
using hipObjTest::setReplyWithPeerToken;
#endif /* HIPOBJ_RDMA_WIRE_V1 */

void *const kDevBuf = hipObjTest::fakeDevBuf();

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

/* The modify_qp() masks for each transition */
constexpr int kRtrMask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
constexpr int kRtsMask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC;

class TransportTest : public hipObjTest::FakeDeviceTest {
protected:
    /* Opens the fake device and creates a QP on it */
    static void open(hipObj::RcConnection &conn)
    {
        ASSERT_EQ(hipObj::openRdmaDevice(0, conn), 0);
        ASSERT_EQ(hipObj::createRcQp(conn, 16, 8, 8), 0);
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
};

// ---- Opening the device ------------------------------------------

TEST_F(TransportTest, OpenSetsUpContextAndPd)
{
    hipObj::RcConnection conn;
    ASSERT_EQ(hipObj::openRdmaDevice(0, conn), 0);
    EXPECT_TRUE(conn.ctx);
    EXPECT_TRUE(conn.pd);
    /* The fake port has no GIDs, so the first one is used */
    EXPECT_EQ(conn.gidIndex, 0);
}

TEST_F(TransportTest, OpenRejectsIndexOutsideDeviceList)
{
    for (int index : {-1, 1}) {
        hipObj::RcConnection conn;
        EXPECT_EQ(hipObj::openRdmaDevice(index, conn), -1) << "index " << index;
    }
    EXPECT_EQ(fake().log.openDevice, 0);
}

TEST_F(TransportTest, OpenFailsWithoutDeviceList)
{
    fake().fail.getDeviceList = true;
    hipObj::RcConnection conn;
    EXPECT_EQ(hipObj::openRdmaDevice(0, conn), -1);
    EXPECT_EQ(hipObj::openRdmaDeviceByName("mlx5_0", conn), -1);
}

TEST_F(TransportTest, OpenByNameFindsOnlyThatDevice)
{
    hipObj::RcConnection conn;
    EXPECT_EQ(hipObj::openRdmaDeviceByName(nullptr, conn), -1);
    EXPECT_EQ(hipObj::openRdmaDeviceByName("mlx5_1", conn), -1);
    EXPECT_EQ(fake().log.openDevice, 0);

    EXPECT_EQ(hipObj::openRdmaDeviceByName("mlx5_0", conn), 0);
}

TEST_F(TransportTest, OpenReleasesWhatItAcquiredWhenAStepFails)
{
    for (bool hipObjTest::Faults::*fault : {&hipObjTest::Faults::openDevice, &hipObjTest::Faults::allocPd,
                                            &hipObjTest::Faults::queryPort, &hipObjTest::Faults::queryGid}) {
        resetFake();
        fake().fail.*fault = true;
        {
            hipObj::RcConnection conn;
            EXPECT_EQ(hipObj::openRdmaDevice(0, conn), -1);
            EXPECT_FALSE(conn.ctx);
            EXPECT_FALSE(conn.pd);
        }
        expectNothingLeaked();
    }
}

// ---- Creating the QP ---------------------------------------------

TEST_F(TransportTest, CreateQpReleasesCqWhenQpCreationFails)
{
    {
        hipObj::RcConnection conn;
        ASSERT_EQ(hipObj::openRdmaDevice(0, conn), 0);
        fake().fail.createQp = true;
        EXPECT_EQ(hipObj::createRcQp(conn, 16, 8, 8), -1);
        EXPECT_FALSE(conn.qp);
        EXPECT_FALSE(conn.cq);
        EXPECT_EQ(fake().log.destroyCq, 1);

        fake().fail.createCq = true;
        EXPECT_EQ(hipObj::createRcQp(conn, 16, 8, 8), -1);
    }
    expectNothingLeaked();
}

TEST_F(TransportTest, DestroyingReleasesEverything)
{
    {
        hipObj::RcConnection conn;
        ASSERT_NO_FATAL_FAILURE(open(conn));
        EXPECT_EQ(fake().log.destroyQp, 0);
    }
    EXPECT_EQ(fake().log.destroyQp, 1);
    expectNothingLeaked();
}

TEST_F(TransportTest, InitTransitionGrantsRemoteReadAndWrite)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    ASSERT_EQ(hipObj::transitionQpToInit(conn), 0);

    std::vector<hipObjTest::ModifyQpCall> calls = modifyQpCalls();
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_EQ(calls[0].attr.qp_state, IBV_QPS_INIT);
    EXPECT_EQ(calls[0].attr.port_num, conn.portNum);
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
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    const hipObj::RdmaToken token = peerToken();
    ASSERT_EQ(hipObj::connectRcPeer(conn, token), 0);

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
    EXPECT_EQ(rtr.ah_attr.port_num, conn.portNum);
    EXPECT_EQ(rtr.ah_attr.grh.sgid_index, conn.gidIndex);
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

TEST_F(TransportTest, ConnectRejectsPeerThatIsNotRc)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    EXPECT_EQ(hipObj::connectRcPeer(conn, peerToken(hipObj::TransportType::DC)), -1);
    EXPECT_EQ(fake().modifyQpCount, 0U);
}

TEST_F(TransportTest, ConnectNeedsQp)
{
    hipObj::RcConnection conn;
    ASSERT_EQ(hipObj::openRdmaDevice(0, conn), 0);
    EXPECT_EQ(hipObj::connectRcPeer(conn, peerToken()), -1);
    EXPECT_EQ(fake().modifyQpCount, 0U);
}

TEST_F(TransportTest, ConnectStopsAtFailedTransition)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    fake().fail.modifyQpToState = IBV_QPS_RTR;
    EXPECT_NE(hipObj::connectRcPeer(conn, peerToken()), 0);
    EXPECT_EQ(fake().modifyQpCount, 1U);

    fake().modifyQpCount        = 0;
    fake().fail.modifyQpToState = IBV_QPS_RTS;
    EXPECT_NE(hipObj::connectRcPeer(conn, peerToken()), 0);
    EXPECT_EQ(fake().modifyQpCount, 2U);
}

TEST_F(TransportTest, RtrRejectsGidIndexTheAddressHandleCannotHold)
{
    /* The address handle holds the GID index in a uint8_t, so anything
     * outside [0, 255] must be refused, not truncated */
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    ibv_gid gid{};
    for (int index : {-1, 256, 0x10000}) {
        conn.gidIndex = index;
        EXPECT_EQ(hipObj::transitionQpToRtr(conn, 1, 0, gid), -1) << "index " << index;
    }
    EXPECT_EQ(fake().modifyQpCount, 0U);

    conn.gidIndex = 255;
    ASSERT_EQ(hipObj::transitionQpToRtr(conn, 1, 0, gid), 0);
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
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    fake().vendorId = GetParam().vendorId;
    ASSERT_EQ(hipObj::connectRcPeer(conn, peerToken()), 0);

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
    {"Broadcom", hipObj::VENDOR_ID_BROADCOM, true},
#endif
#ifdef HIPOBJ_IONIC
    {"Pensando", hipObj::VENDOR_ID_PENSANDO, true},
#endif
    {"None", 0, false},
});

INSTANTIATE_TEST_SUITE_P(Transport, VendorQpTest, ::testing::ValuesIn(kVendorCases),
                         [](const ::testing::TestParamInfo<VendorCase> &paramInfo) {
                             return paramInfo.param.name;
                         });

TEST_F(TransportTest, NoVendorAttributesWhenDeviceQueryFails)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    fake().vendorId         = hipObj::VENDOR_ID_BROADCOM;
    fake().fail.queryDevice = true;
    ASSERT_EQ(hipObj::connectRcPeer(conn, peerToken()), 0);
    EXPECT_EQ(modifyQpCalls().at(0).attr.timeout, 0);
}

// ---- Polling for the completion ----------------------------------

TEST_F(TransportTest, PollReturnsOnCompletion)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    EXPECT_EQ(hipObj::pollCompletion(conn, -1, std::chrono::seconds(1)), 0);
    fake().pollOpcode = IBV_WC_RDMA_READ;
    EXPECT_EQ(hipObj::pollCompletion(conn, IBV_WC_RDMA_READ, std::chrono::seconds(1)), 0);
}

TEST_F(TransportTest, PollNeedsCq)
{
    hipObj::RcConnection conn;
    EXPECT_EQ(hipObj::pollCompletion(conn, -1, std::chrono::seconds(1)), -1);
}

TEST_F(TransportTest, PollFailsOnErrorCompletion)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    fake().pollStatus = IBV_WC_REM_ACCESS_ERR;
    EXPECT_EQ(hipObj::pollCompletion(conn, -1, std::chrono::seconds(1)), -1);
}

TEST_F(TransportTest, PollFailsWhenPollingFails)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    fake().pollResult = -1;
    EXPECT_EQ(hipObj::pollCompletion(conn, -1, std::chrono::seconds(1)), -1);
}

TEST_F(TransportTest, PollTimesOutWithoutTheAwaitedCompletion)
{
    hipObj::RcConnection conn;
    ASSERT_NO_FATAL_FAILURE(open(conn));
    /* Nothing completes */
    fake().pollResult = 0;
    EXPECT_EQ(hipObj::pollCompletion(conn, -1, std::chrono::milliseconds(10)), -1);
    /* Only other work completes */
    fake().pollResult = 1;
    fake().pollOpcode = IBV_WC_SEND;
    EXPECT_EQ(hipObj::pollCompletion(conn, IBV_WC_RDMA_READ, std::chrono::milliseconds(10)), -1);
}

// ---- hipObjInit --------------------------------------------------

TEST_F(TransportTest, InitFailsAndReleasesEverythingWhenSetupFails)
{
    for (bool hipObjTest::Faults::*fault : {
             &hipObjTest::Faults::getDeviceList,
             &hipObjTest::Faults::openDevice,
             &hipObjTest::Faults::allocPd,
             &hipObjTest::Faults::queryPort,
             &hipObjTest::Faults::queryGid,
#ifdef HIPOBJ_RDMA_WIRE_V1
             /* Only v1's hipObjInit() creates a CQ and a QP */
             &hipObjTest::Faults::createCq,
             &hipObjTest::Faults::createQp,
#endif
         }) {
        resetFake();
        fake().fail.*fault    = true;
        hipObjConfig_t config = makeConfig();
        EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
        EXPECT_EQ(hipObj::currentInstance(), nullptr);
        expectNothingLeaked();
    }
}

#ifdef HIPOBJ_RDMA_WIRE_V1
TEST_F(TransportTest, InitFailsAndReleasesEverythingWhenInitTransitionFails)
{
    fake().fail.modifyQpToState = IBV_QPS_INIT;
    hipObjConfig_t config       = makeConfig();
    EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    expectNothingLeaked();
}
#endif /* HIPOBJ_RDMA_WIRE_V1 */

TEST_F(TransportTest, InitFailsForUnknownNic)
{
    hipObjConfig_t config = makeConfig();
    config.nicHint        = "mlx5_1";
    EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
}

TEST_F(TransportTest, ShutdownReleasesEverything)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
#ifdef HIPOBJ_RDMA_WIRE_V1
    EXPECT_EQ(fake().log.createQp, 1);
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
failSendRequest(void *, const char *, size_t)
{
    return -1;
}

int
failRecvReply(void *, char *, size_t *)
{
    return 5;
}

#ifdef HIPOBJ_RDMA_WIRE_V1
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
#endif /* HIPOBJ_RDMA_WIRE_V1 */

class TransportTransferTest : public TransportTest, public ::testing::WithParamInterface<TransferFn> {
protected:
    hipObjError_t transfer()
    {
        hipObjOps_t ops = makeOps();
        return GetParam()(&ops);
    }
};

#ifdef HIPOBJ_RDMA_WIRE_V1
TEST_P(TransportTransferTest, ConnectsToPeerInReply)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    setReplyWithPeerToken(peerToken());
    ASSERT_EQ(transfer().opError, hipObjSuccess);

    /* hipObjInit() moved the QP to INIT; the reply's token connects it */
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
#endif /* HIPOBJ_RDMA_WIRE_V1 */

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
#ifdef HIPOBJ_RDMA_WIRE_V1
    EXPECT_EQ(fake().modifyQpCount, 1U);
#endif
}

#ifndef HIPOBJ_RDMA_WIRE_V1
/* A transfer succeeds only when the server moved the whole range, since
 * hipObjGet() and hipObjPut() can't report that fewer bytes moved */
TEST_P(TransportTransferTest, SucceedsOnlyWhenTheWholeRangeMoved)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    for (const uint64_t bytes : {uint64_t{0}, uint64_t{kBufSize - 1}, uint64_t{kBufSize + 1},
                                 std::numeric_limits<uint64_t>::max()}) {
        setReply(hipObj::v3::encodeRdmaReplyV3({200, bytes}));
        EXPECT_EQ(transfer().opError, hipObjS3Error) << bytes << " bytes";
    }
    setReply(hipObj::v3::encodeRdmaReplyV3({200, kBufSize}));
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
    hipObjOps_t ops = makeOps();
    ops.sendRequest = &recordV3SendRequest;
    std::optional<hipObj::v3::RdmaTokenV3> sent;
    ASSERT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, &sent).opError, hipObjSuccess);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent->listenIp, "192.0.2.1");
    EXPECT_EQ(sent->listenPort, fake().listenerPort);
    EXPECT_EQ(fake().log.startListener, 1);
}

/* The server dials the listener, so without one no transfer could work:
 * hipObjInit() fails, and releases what it acquired, when the NIC has no
 * IPv4 address to bind to, the listener doesn't start, or its bind resolves
 * to another NIC */
TEST_F(TransportTest, InitFailsWhenTheListenerCannotStart)
{
    hipObjConfig_t config = makeConfig();

    fake().gid = {};
    EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.startListener, 0);

    fake().gid                = hipObjTest::kFakeNicGid;
    fake().fail.startListener = true;
    EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.startListener, 1);

    fake().fail.startListener = false;
    fake().listenerDevice     = "mlx5_1";
    EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.startListener, 2);
    EXPECT_EQ(fake().log.stopListener, 1);

    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    expectNothingLeaked();
}
#endif /* HIPOBJ_RDMA_WIRE_V1 */

#ifdef HIPOBJ_RDMA_WIRE_V1
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
#endif /* HIPOBJ_RDMA_WIRE_V1 */

INSTANTIATE_TEST_SUITE_P(Transport, TransportTransferTest, ::testing::Values(&callGet, &callPut),
                         [](const ::testing::TestParamInfo<TransferFn> &paramInfo) {
                             return paramInfo.param == &callGet ? "Get" : "Put";
                         });

} // namespace
