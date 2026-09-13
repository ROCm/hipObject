/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Mock-consumer tests for the v2 client driver (v2Transfer): the
 * full PREPARE -> READY -> data -> FINAL path is driven through the
 * hipObjOpsV2_t callbacks with faked verbs and a scriptable consumer,
 * asserting the callback/data interleaving and the deadline fields
 * the library reports. No RDMA or GPU hardware is involved. */

#include <arpa/inet.h>

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hip-seam.h"
#include "../../../src/common/ibv-wrapper.h"
#include "../../../src/common/nic-seam.h"
#include "../../../src/rdma/token.h"
#include "../../../src/rdma/v2-client.h"
#include "../../../src/rdma/v2-registry.h"
#include "hipobj.h"

namespace {

/* ---- fake verbs objects ------------------------------------------------ */

struct FakeQp {
  uint32_t qp_num;
};
struct FakeCq {
  int magic = 0x4351;
  /* Completions the next poll_cq calls return, in order. */
  std::vector<struct ibv_wc> pending;
};

/* Side-channel handles the consumer mock uses to script the data
 * phase: the most recent objects the fakes handed the library. */
FakeCq* g_lastCq = nullptr;
uint32_t g_lastQpn = 0;

/* Fake device/context/pd: addresses only, never dereferenced as
 * real verbs objects (all verbs entry points are faked). */
struct ibv_device* g_fakeDevList[2] = {nullptr, nullptr};
struct ibv_context g_fakeCtx;
struct ibv_pd g_fakePd;

int g_postRecvCalls = 0;
int g_postSendCalls = 0;
const struct ibv_recv_wr* g_lastRecvWr = nullptr;
const struct ibv_send_wr* g_lastSendWr = nullptr;
uint32_t g_cookieInImm = 0;

int fakePostRecv(struct ibv_qp*, struct ibv_recv_wr* wr,
                 struct ibv_recv_wr**) {
  ++g_postRecvCalls;
  g_lastRecvWr = wr;
  return 0;
}

int fakePostSend(struct ibv_qp*, struct ibv_send_wr* wr,
                 struct ibv_send_wr**) {
  ++g_postSendCalls;
  g_lastSendWr = wr;
  g_cookieInImm = ntohl(wr->imm_data);
  return 0;
}

int fakePollCq(struct ibv_cq* cq, int, struct ibv_wc* wc) {
  FakeCq* f = reinterpret_cast<FakeCq*>(cq);
  if (f->pending.empty()) {
    return 0;
  }
  *wc = f->pending.front();
  f->pending.erase(f->pending.begin());
  return 1;
}

struct ibv_qp* fakeCreateQp(struct ibv_pd*, struct ibv_qp_init_attr*) {
  static uint32_t nextQpn = 0x2000;
  const uint32_t qpn = nextQpn++;
  g_lastQpn = qpn;
  return reinterpret_cast<struct ibv_qp*>(new FakeQp{qpn});
}

struct ibv_cq* fakeCreateCq(struct ibv_context*, int, void*,
                            struct ibv_comp_channel*, int) {
  auto* cq = new FakeCq();
  g_lastCq = cq;
  return reinterpret_cast<struct ibv_cq*>(cq);
}

int fakeDestroyQp(struct ibv_qp* qp) {
  delete reinterpret_cast<FakeQp*>(qp);
  return 0;
}

int fakeDestroyCq(struct ibv_cq* cq) {
  delete reinterpret_cast<FakeCq*>(cq);
  return 0;
}

struct ibv_mr* fakeRegMr(struct ibv_pd*, void* addr, size_t, int) {
  auto* mr = new struct ibv_mr();
  std::memset(mr, 0, sizeof(*mr));
  mr->addr = addr;
  mr->lkey = 0x11223344;
  mr->rkey = 0x55667788;
  return mr;
}

int fakeDeregMr(struct ibv_mr* mr) {
  delete mr;
  return 0;
}

int fakeModifyQp(struct ibv_qp*, struct ibv_qp_attr*, int) {
  return 0;
}

/* ---- NIC seam ---------------------------------------------------------- */

class FakeNics : public hipObj::NicEnumerator {
public:
  std::vector<hipObj::NicInfo> Enumerate(const char*) override {
    hipObj::NicInfo info;
    info.dev_name = "fake0";
    info.pcie_bus_id = "0000:42:00.0";
    info.port_num = 1;
    return {info};
  }
};

/* ---- mock consumer ------------------------------------------------------ */

enum class Cb { Prepare, ReadyRequest, FinishReady, Cancel };

struct CallRecord {
  Cb cb;
  hipObjTransferReqV2_t req; /* copied snapshot */
};

class MockConsumer {
public:
  /* Scripted PREPARE reply. */
  hipObjPrepareReplyV2_t prep;
  /* Scripted FINAL reply. */
  hipObjFinalReplyV2_t fin;
  /* Non-zero: the callback returns it (consumer-side failure). */
  int prepareFail = 0;
  int readyRequestFail = 0;
  int finishReadyFail = 0;
  int cancelCalls = 0;
  std::vector<CallRecord> calls;

  /* When set, sendReadyRequest arms the fake CQ with a data-phase
   * completion (GET receive form) so the data phase succeeds. */
  bool armGetCompletion = false;
  bool armPutCompletion = false;

  void install(hipObjOpsV2_t* ops) {
    ops->sendPrepare = [](void* ctx, const hipObjTransferReqV2_t* req,
                          hipObjPrepareReplyV2_t* out) -> int {
      auto* self = static_cast<MockConsumer*>(ctx);
      self->snapshot(Cb::Prepare, req);
      if (self->prepareFail != 0) {
        return self->prepareFail;
      }
      *out = self->prep;
      return 0;
    };
    ops->sendReadyRequest = [](void* ctx,
                               const hipObjTransferReqV2_t* req) -> int {
      auto* self = static_cast<MockConsumer*>(ctx);
      self->snapshot(Cb::ReadyRequest, req);
      if (g_lastCq != nullptr && self->armGetCompletion) {
        struct ibv_wc wc = {};
        wc.status = IBV_WC_SUCCESS;
        wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
        wc.wc_flags = IBV_WC_WITH_IMM;
        wc.imm_data = htonl(req->cookie);
        g_lastCq->pending.push_back(wc);
      }
      if (g_lastCq != nullptr && self->armPutCompletion) {
        struct ibv_wc wc = {};
        wc.status = IBV_WC_SUCCESS;
        wc.opcode = IBV_WC_RDMA_WRITE;
        g_lastCq->pending.push_back(wc);
      }
      return self->readyRequestFail;
    };
    ops->finishReady = [](void* ctx, const hipObjTransferReqV2_t* req,
                          hipObjFinalReplyV2_t* out) -> int {
      auto* self = static_cast<MockConsumer*>(ctx);
      self->snapshot(Cb::FinishReady, req);
      if (self->finishReadyFail != 0) {
        return self->finishReadyFail;
      }
      *out = self->fin;
      return 0;
    };
    ops->sendCancel = [](void* ctx, const hipObjTransferReqV2_t* req) -> int {
      auto* self = static_cast<MockConsumer*>(ctx);
      self->snapshot(Cb::Cancel, req);
      ++self->cancelCalls;
      return 0;
    };
  }

  const CallRecord* find(Cb cb) const {
    for (const auto& c : calls) {
      if (c.cb == cb) {
        return &c;
      }
    }
    return nullptr;
  }

private:
  void snapshot(Cb cb, const hipObjTransferReqV2_t* req) {
    CallRecord r;
    r.cb = cb;
    r.req = *req; /* string fields alias library-owned storage */
    calls.push_back(r);
  }
};

/* ---- fixture ------------------------------------------------------------ */

class V2ClientTransferTest : public ::testing::Test {
protected:
  void SetUp() override {
    savedHipOps_ = hipObj::hipOps();
    hipObj::HipOps hops;
    hops.hipDeviceGetPCIBusId = [](char* bus, int len, int) -> hipError_t {
      snprintf(bus, static_cast<size_t>(len), "0000:42:00.0");
      return hipSuccess;
    };
    hipObj::hipOps() = hops;

    savedFuncs_ = hipObj::ibv.funcsForTest();
    auto& f = hipObj::ibv.funcsForTest();
    f.get_device_list = [](int* n) -> struct ibv_device** {
      *n = 1;
      return g_fakeDevList;
    };
    f.free_device_list = [](struct ibv_device**) {};
    f.open_device = [](struct ibv_device*) -> struct ibv_context* {
      return &g_fakeCtx;
    };
    f.alloc_pd = [](struct ibv_context*) -> struct ibv_pd* {
      return &g_fakePd;
    };
    f.dealloc_pd = [](struct ibv_pd*) -> int { return 0; };
    f.query_port = [](struct ibv_context*, uint8_t, struct ibv_port_attr* a) {
      a->state = IBV_PORT_ACTIVE;
      a->lid = 1;
      return 0;
    };
    f.query_gid = [](struct ibv_context*, uint8_t, int, union ibv_gid* g) {
      std::memset(g, 0xcd, sizeof(*g));
      return 0;
    };
    f.query_device = [](struct ibv_context*, struct ibv_device_attr* a) {
      std::memset(a, 0, sizeof(*a));
      a->max_mr_size = ~(0ULL);
      return 0;
    };
    f.dealloc_qp...[truncated]
    f.create_qp = fakeCreateQp;
    f.destroy_qp = fakeDestroyQp;
    f.create_cq = fakeCreateCq;
    f.destroy_cq = fakeDestroyCq;
    f.modify_qp = fakeModifyQp;
    f.post_recv = fakePostRecv;
    f.post_send = fakePostSend;
    f.poll_cq = fakePollCq;
    f.reg_mr = fakeRegMr;
    f.dereg_mr = fakeDeregMr;
    hipObj::ibv.is_initialized = true;

    savedNics_ = hipObj::setNicEnumerator(&nics_);

    g_postRecvCalls = 0;
    g_postSendCalls = 0;
    g_lastRecvWr = nullptr;
    g_lastSendWr = nullptr;
    g_lastCq = nullptr;

    ASSERT_EQ(initV2("http://s3.example:9000", 0), hipObjSuccess);
    ASSERT_EQ(hipObjBufRegister(buf_, kBufSize).opError, hipObjSuccess);

    /* Valid server PREPARE reply: an RC token with a nonzero GID. */
    hipObj::RdmaToken tok{};
    tok.qpNum = 0x9999;
    std::memset(tok.gid, 0xab, sizeof(tok.gid));
    tok.transport = hipObj::TRANSPORT_RC;
    tok.portNum = 1;
    const std::string enc = hipObj::encodeRdmaToken(tok);
    std::memcpy(consumer_.prep.serverToken, enc.c_str(),
                enc.size() < sizeof(consumer_.prep.serverToken)
                  ? enc.size()
                  : sizeof(consumer_.prep.serverToken) - 1);
    std::strncpy(consumer_.prep.session, "00112233445566778899aabbccddeeff",
                 sizeof(consumer_.prep.session) - 1);
    consumer_.prep.httpStatus = 200;
    consumer_.prep.serverPsn = 5;
    consumer_.prep.stagingAddr = 0x70000000;
    consumer_.prep.stagingRkey = 0x21436587;
    consumer_.prep.stagingPresent = 1;

    consumer_.fin.httpStatus = 200; /* GET success */
    consumer_.fin.cookiePresent = 1;

    std::memset(&ops_, 0, sizeof(ops_));
    consumer_.install(&ops_);
  }

  void TearDown() override {
    EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
    hipObj::hipOps() = savedHipOps_;
    hipObj::ibv.funcsForTest() = savedFuncs_;
    hipObj::setNicEnumerator(savedNics_);
    hipObj::ibv.is_initialized = false;
  }

  hipObjOpError_t initV2(const char* endpoint, uint32_t transferMs) {
    hipObjConfigV2_t cfg = {};
    cfg.v1.gpuDevice = 7;
    cfg.control.controlEndpoint = endpoint;
    cfg.transferDeadlineMs = transferMs;
    return hipObjInitV2(&cfg).opError;
  }

  static constexpr size_t kBufSize = 4096;
  alignas(4096) char buf_[kBufSize];
  hipObjOpsV2_t ops_{};
  MockConsumer consumer_;
  FakeNics nics_;
  hipObj::HipOps savedHipOps_;
  hipObj::IbvFuncs savedFuncs_;
  hipObj::NicEnumerator* savedNics_ = nullptr;
};

/* ---- happy-path interleaving (GET) -------------------------------------- */

/* The library must call sendPrepare, then sendReadyRequest, then
 * post the receive, then the data phase, then finishReady, with the
 * READY request carrying the wire endpoint fields. */
TEST_F(V2ClientTransferTest, GetCallbackDataInterleaving) {
  consumer_.armGetCompletion = true;

  const hipObjError_t err =
    hipObjGetV2("bkt", "obj", buf_, 512, 0, nullptr, &ops_, &consumer_);
  ASSERT_EQ(err.opError, hipObjSuccess);

  /* Interleaving: Prepare, ReadyRequest, (receive posted), FinishReady. */
  ASSERT_EQ(consumer_.calls.size(), 3u);
  EXPECT_EQ(consumer_.calls[0].cb, Cb::Prepare);
  EXPECT_EQ(consumer_.calls[1].cb, Cb::ReadyRequest);
  EXPECT_EQ(consumer_.calls[2].cb, Cb::FinishReady);
  EXPECT_EQ(g_postRecvCalls, 1) << "GET posts the receive once";
  EXPECT_EQ(g_postSendCalls, 0) << "GET never posts a send";

  /* The READY request carries the wire endpoint fields. */
  const CallRecord* ready = consumer_.find(Cb::ReadyRequest);
  ASSERT_NE(ready, nullptr);
  EXPECT_EQ(ready->req.clientQpn, g_lastQpn);
  EXPECT_EQ(ready->req.clientMrAddr, reinterpret_cast<uint64_t>(buf_));
  EXPECT_EQ(ready->req.clientMrRkey, 0x55667788u);
  ASSERT_NE(ready->req.session, nullptr);
  EXPECT_STREQ(ready->req.session, "00112233445566778899aabbccddeeff");
  ASSERT_NE(ready->req.endpoint, nullptr);
  EXPECT_STREQ(ready->req.endpoint->controlEndpoint, "http://s3.example:9000");

  /* The deadline fields describe the remaining budget, not an
   * absolute clock value (60 s default budget bounds both). */
  EXPECT_LT(ready->req.deadlineMs, 60000u);
  EXPECT_GT(ready->req.deadlineMs, 0u);
  EXPECT_EQ(ready->req.deadlineMs, ready->req.remainingMs);

  /* No CANCEL on the success path. */
  EXPECT_EQ(consumer_.cancelCalls, 0);
}

/* ---- happy-path interleaving (PUT) -------------------------------------- */

TEST_F(V2ClientTransferTest, PutCallbackDataInterleaving) {
  consumer_.fin.httpStatus = 204; /* PUT success */
  consumer_.armPutCompletion = true;

  const hipObjError_t err =
    hipObjPutV2("bkt", "obj", buf_, 512, 0, nullptr, &ops_, &consumer_);
  ASSERT_EQ(err.opError, hipObjSuccess);

  ASSERT_EQ(consumer_.calls.size(), 3u);
  EXPECT_EQ(g_postSendCalls, 1) << "PUT posts one RDMA WRITE";
  EXPECT_EQ(g_postRecvCalls, 0);
  ASSERT_NE(g_lastSendWr, nullptr);
  EXPECT_EQ(g_lastSendWr->opcode, IBV_WR_RDMA_WRITE_WITH_IMM);
  EXPECT_EQ(g_lastSendWr->wr.rdma.remote_addr, 0x70000000u);
  EXPECT_EQ(g_lastSendWr->wr.rdma.rkey, 0x21436587u);
  EXPECT_EQ(g_cookieInImm, consumer_.calls[0].req.cookie)
    << "WRITE immediate data carries the client cookie";
}

/* PUT without a staging advertisement fails before READY. */
TEST_F(V2ClientTransferTest, PutWithoutStagingFailsBeforeReady) {
  consumer_.prep.stagingPresent = 0;

  const hipObjError_t err =
    hipObjPutV2("bkt", "obj", buf_, 512, 0, nullptr, &ops_, &consumer_);
  EXPECT_EQ(err.opError, hipObjInvalidValue);
  EXPECT_EQ(consumer_.find(Cb::ReadyRequest), nullptr)
    << "no READY after a rejected staging advertisement";
}

/* ---- failure paths ------------------------------------------------------ */

/* PREPARE 501 + unsupported marker maps to NotSupported and never
 * reaches READY or the data phase. */
TEST_F(V2ClientTransferTest, Prepare501StopsBeforeReady) {
  consumer_.prep.httpStatus = 501;
  consumer_.prep.unsupportedMarker = 1;

  const hipObjError_t err =
    hipObjGetV2("bkt", "obj", buf_, 512, 0, nullptr, &ops_, &consumer_);
  EXPECT_EQ(err.opError, hipObjNotSupported);
  EXPECT_EQ(consumer_.find(Cb::ReadyRequest), nullptr);
  EXPECT_EQ(g_postRecvCalls, 0);
  EXPECT_EQ(consumer_.cancelCalls, 0)
    << "nothing was published; no CANCEL";
}

/* A data-phase expiry after a successful READY request must attempt
 * exactly one CANCEL carrying the fresh cleanup budget, not the
 * exhausted transfer budget. */
TEST_F(V2ClientTransferTest, DataExpiryIssuesSingleCancel) {
  /* Re-init with a tiny transfer budget so the poll deadline expires
   * quickly instead of stalling the test for the 60 s default. */
  ASSERT_EQ(hipObjShutdown().opError, hipObjSuccess);
  ASSERT_EQ(initV2("http://s3.example:9000", 150), hipObjSuccess);
  ASSERT_EQ(hipObjBufRegister(buf_, kBufSize).opError, hipObjSuccess);
  /* No completion is armed: the data phase runs out of budget. */
  consumer_.armGetCompletion = false;

  const hipObjError_t err =
    hipObjGetV2("bkt", "obj", buf_, 512, 0, nullptr, &ops_, &consumer_);
  EXPECT_EQ(err.opError, hipObjBusy) << "expiry maps to Busy per the driver";

  /* Exactly one CANCEL, carrying the session id and the fresh
   * cleanup budget (default 1 s). */
  ASSERT_EQ(consumer_.cancelCalls, 1);
  const CallRecord* cancel = consumer_.find(Cb::Cancel);
  ASSERT_NE(cancel, nullptr);
  ASSERT_NE(cancel->req.session, nullptr);
  EXPECT_STREQ(cancel->req.session, "00112233445566778899aabbccddeeff");
  EXPECT_EQ(cancel->req.remainingMs, 1000u);
}

/* finishReady reporting a FINAL whose cookie echo does not match
 * fails the transfer with RdmaError. */
TEST_F(V2ClientTransferTest, FinalCookieMismatchFails) {
  consumer_.armGetCompletion = true;
  consumer_.fin.cookieEcho = 0xdeadbeef; /* will not match */

  const hipObjError_t err =
    hipObjGetV2("bkt", "obj", buf_, 512, 0, nullptr, &ops_, &consumer_);
  EXPECT_EQ(err.opError, hipObjRdmaError);
}

/* Buffer admission: an unregistered pointer is rejected before any
 * callback fires. */
TEST_F(V2ClientTransferTest, UnregisteredBufferRejected) {
  alignas(4096) char unreg[512];
  const hipObjError_t err =
    hipObjGetV2("bkt", "obj", unreg, 512, 0, nullptr, &ops_, &consumer_);
  EXPECT_EQ(err.opError, hipObjBufNotRegistered);
  EXPECT_TRUE(consumer_.calls.empty())
    << "no callback before buffer admission";
}

} // namespace
