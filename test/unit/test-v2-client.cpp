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
#include <map>
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

/* Real verbs structs are allocated (never handed to real verbs) so
 * field reads like qp->qp_num see the true layout; the completion
 * queue keeps its scripted completions in a side map keyed by the
 * struct address. */
struct FakeCqState {
  /* Completions the next poll_cq calls return, in order. */
  std::vector<struct ibv_wc> pending;
};
std::map<struct ibv_cq*, FakeCqState> g_cqStates;

/* Side-channel handles the consumer mock uses to script the data
 * phase: the most recent objects the fakes handed the library. */
struct ibv_cq* g_lastCq = nullptr;
uint32_t g_lastQpn = 0;

/* Fake device/context/pd: real struct layouts (field reads like
 * qp->qp_num and dev->name must see the true offsets). */
struct ibv_device* g_fakeDevList[2] = {nullptr, nullptr};
struct ibv_context g_fakeCtx;
struct ibv_pd g_fakePd;

/* Allocate the single fake device entry with name "fake0" so
 * openRdmaDeviceByName's name scan matches it. */
struct ibv_device* allocFakeDevice(const char* name) {
  auto* dev = new struct ibv_device();
  std::memset(dev, 0, sizeof(*dev));
  snprintf(dev->name, sizeof(dev->name), "%s", name);
  return dev;
}

int g_postRecvCalls = 0;
int g_postSendCalls = 0;
const struct ibv_recv_wr* g_lastRecvWr = nullptr;
struct ibv_send_wr g_ownedSendWr;
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
  g_ownedSendWr = *wr; /* the library's wr is stack-scoped */
  g_lastSendWr = &g_ownedSendWr;
  g_cookieInImm = ntohl(wr->imm_data);
  return 0;
}

int fakePollCq(struct ibv_cq* cq, int, struct ibv_wc* wc) {
  auto it = g_cqStates.find(cq);
  if (it == g_cqStates.end() || it->second.pending.empty()) {
    return 0;
  }
  *wc = it->second.pending.front();
  it->second.pending.erase(it->second.pending.begin());
  return 1;
}

struct ibv_qp* fakeCreateQp(struct ibv_pd*, struct ibv_qp_init_attr*) {
  static uint32_t nextQpn = 0x2000;
  auto* qp = new struct ibv_qp();
  std::memset(qp, 0, sizeof(*qp));
  qp->qp_num = nextQpn++;
  g_lastQpn = qp->qp_num;
  return qp;
}

struct ibv_cq* fakeCreateCq(struct ibv_context*, int, void*,
                            struct ibv_comp_channel*, int) {
  auto* cq = new struct ibv_cq();
  std::memset(cq, 0, sizeof(*cq));
  g_cqStates[cq] = FakeCqState{};
  g_lastCq = cq;
  return cq;
}

int fakeDestroyQp(struct ibv_qp* qp) {
  delete qp;
  return 0;
}

int fakeDestroyCq(struct ibv_cq* cq) {
  g_cqStates.erase(cq);
  delete cq;
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
  /* Non-zero: finishReady echoes this value instead of the real
   * cookie (cookie-mismatch test). */
  uint32_t cookieEchoOverride = 0;

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
        auto it = g_cqStates.find(g_lastCq);
        if (it != g_cqStates.end()) {
        struct ibv_wc wc = {};
        wc.status = IBV_WC_SUCCESS;
        wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
        wc.wc_flags = IBV_WC_WITH_IMM;
          wc.imm_data = htonl(req->cookie);
          it->second.pending.push_back(wc);
        }
      }
      if (g_lastCq != nullptr && self->armPutCompletion) {
        auto it = g_cqStates.find(g_lastCq);
        if (it != g_cqStates.end()) {
          struct ibv_wc wc = {};
          wc.status = IBV_WC_SUCCESS;
          wc.opcode = IBV_WC_RDMA_WRITE;
          it->second.pending.push_back(wc);
        }
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
      /* Echo the cookie the library generated (visible on the
       * PREPARE snapshot) unless the test overrides the echo. */
      if (out->cookiePresent) {
        const CallRecord* prep = self->find(Cb::Prepare);
        if (prep != nullptr && self->cookieEchoOverride == 0) {
          out->cookieEcho = prep->req.cookie;
        }
      }
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
    ownedSession = req->session ? req->session : "";
    r.req.session = ownedSession.c_str();
    ownedTarget = req->target ? req->target : "";
    r.req.target = ownedTarget.c_str();
    calls.push_back(r);
  }

  /* Owns the storage the latest snapshot's string fields point into,
   * so assertions after the transfer returns stay valid. */
  std::string ownedSession;
  std::string ownedTarget;
};

/* ---- fixture ------------------------------------------------------------ */

class V2ClientTransferTest : public ::testing::Test {
protected:
  void SetUp() override {
    savedHipOps_ = hipObj::hipOps();
    hipObj::HipOps hops;
    hops.hipGetDevice = [](int* device) -> hipError_t {
      *device = 7;
      return hipSuccess;
    };
    hops.hipDeviceGetPCIBusId = [](char* bus, int len, int) -> hipError_t {
      snprintf(bus, static_cast<size_t>(len), "0000:42:00.0");
      return hipSuccess;
    };
    hipObj::hipOps() = hops;

    savedFuncs_ = hipObj::ibv.funcsForTest();
    auto& f = hipObj::ibv.funcsForTest();
    delete g_fakeDevList[0];
    g_fakeDevList[0] = allocFakeDevice("fake0");
    std::memset(&g_fakeCtx, 0, sizeof(g_fakeCtx));
    g_fakeCtx.device = g_fakeDevList[0];
    f.get_device_list = [](int* n) -> struct ibv_device** {
      *n = 1;
      return g_fakeDevList;
    };
    f.free_device_list = [](struct ibv_device**) {};
    f.get_device_name = [](struct ibv_device* dev) -> const char* {
      return dev ? dev->name : nullptr;
    };
    f.close_device = [](struct ibv_context*) -> int { return 0; };
    f.query_port = [](struct ibv_context*, uint8_t,
                      struct ibv_port_attr* attr) -> int {
      std::memset(attr, 0, sizeof(*attr));
      attr->state = IBV_PORT_ACTIVE;
      attr->gid_tbl_len = 1;
      attr->link_layer = IBV_LINK_LAYER_ETHERNET;
      return 0;
    };
    f.query_gid = [](struct ibv_context*, uint8_t, int,
                     union ibv_gid* gid) -> int {
      /* RoCEv2 IPv4-mapped GID so AutoSelectGidIndex picks index 0. */
      std::memset(gid, 0, sizeof(*gid));
      gid->raw[0] = 0xfe;
      gid->raw[1] = 0x80;
      gid->raw[10] = 0xff;
      gid->raw[11] = 0xff;
      gid->raw[12] = 10;   /* 10.0.0.1 */
      gid->raw[13] = 0;
      gid->raw[14] = 0;
      gid->raw[15] = 1;
      return 0;
    };
    f.open_device = [](struct ibv_device*) -> struct ibv_context* {
      return &g_fakeCtx;
    };
    f.alloc_pd = [](struct ibv_context*) -> struct ibv_pd* {
      return &g_fakePd;
    };
    f.dealloc_pd = [](struct ibv_pd*) -> int { return 0; };
    f.query_device = [](struct ibv_context*, struct ibv_device_attr* a) {
      std::memset(a, 0, sizeof(*a));
      a->max_mr_size = ~(0ULL);
      return 0;
    };
    f.create_qp = fakeCreateQp;
    f.destroy_qp = fakeDestroyQp;
    f.create_cq = fakeCreateCq;
    f.destroy_cq = fakeDestroyCq;
    f.modify_qp = fakeModifyQp;
    f.post_recv = fakePostRecv;
    f.post_send = fakePostSend;
    f.poll_cq = fakePollCq;
    f.reg_mr = fakeRegMr;
    f.reg_mr_iova2 = [](struct ibv_pd* pd, void* addr, size_t len,
                        uintptr_t, int) -> struct ibv_mr* {
      return fakeRegMr(pd, addr, len, 0);
    };
    f.dereg_mr = fakeDeregMr;
    hipObj::ibv.is_initialized = true;

    savedNics_ = hipObj::setNicEnumerator(&nics_);

    g_postRecvCalls = 0;
    g_postSendCalls = 0;
    g_lastRecvWr = nullptr;
    std::memset(&g_ownedSendWr, 0, sizeof(g_ownedSendWr));
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
  EXPECT_LE(ready->req.deadlineMs, 60000u);
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
  consumer_.cookieEchoOverride = 0xdeadbeef; /* will not match */

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
