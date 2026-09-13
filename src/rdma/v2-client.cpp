/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* hipobj-rc-v2 client session driver: the implementation behind
 * hipObjInitV2/hipObjGetV2/hipObjPutV2.
 *
 * The driver owns the RDMA session; the consumer owns the control
 * plane (HTTP/SigV4) through the hipObjOpsV2_t callbacks. The READY
 * exchange is split (sendReadyRequest writes the request, the data
 * phase runs while the exchange is pending, finishReady reads the
 * response), so the data motion keeps the overlap the reference
 * client uses while the library stays free of transport code.
 *
 * PUT uses the staging-push model every pinned server implements:
 * the PREPARE reply advertises the server staging MR and the client
 * posts RDMA WRITE WITH IMMEDIATE to it. The pull model (server
 * READs from the client buffer) is proposed but not implemented by
 * any peer today.
 *
 * Everything here runs under v2::apiLock() with no worker threads:
 * helpers join before the entry point returns.
 */

#include "v2-client.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "ibv-wrapper.h"
#include "state.h"
#include "rdma-topology.h"
#include "token.h"
#include "transport.h"
#include "v2-clock.h"
#include "v2-random.h"
#include "v2-registry.h"
#include "v2-state.h"
#include "v2-transport.h"
#include "v2-wire.h"

namespace hipObj {
namespace v2 {

namespace {

constexpr uint32_t kDefaultConnectDeadlineMs = 10'000;
constexpr uint32_t kDefaultTransferDeadlineMs = 60'000;
constexpr uint32_t kDefaultCancelBudgetMs = 1'000;
constexpr uint32_t kPutGraceMs = 100;

uint64_t steadyNowMs() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t remaining(uint64_t deadlineMs) {
  const uint64_t now = steadyNowMs();
  return now >= deadlineMs ? 0 : deadlineMs - now;
}

/* ---- per-process v2 state (guarded by apiLock) ---- */

struct V2State {
  bool initialized = false;
  std::string controlEndpoint;
  uint32_t connectDeadlineMs = 0;
  uint32_t transferDeadlineMs = 0;
  uint32_t cancelBudgetMs = 0;
  /* Shared device handle: created by hipObjInitV2, referenced by every
   * connection. Shutdown reclaims it. */
  DeviceHandle* device = nullptr;
};

V2State& v2State() {
  static V2State state;
  return state;
}

/* ---- deadline-bounded CQ poll ---- */

bool pollDeadline(struct ibv_cq* cq, int expectedOpcode, uint64_t deadlineMs,
                  struct ibv_wc& wc) {
  for (;;) {
    struct ibv_wc cur = {};
    const int n = ibv.poll_cq(cq, 1, &cur);
    if (n > 0) {
      wc = cur;
      return wc.status == IBV_WC_SUCCESS &&
             (expectedOpcode == -1 || wc.opcode == expectedOpcode);
    }
    if (n < 0) {
      return false;
    }
    if (steadyNowMs() >= deadlineMs) {
      return false;
    }
    usleep(1000);
  }
}

/* One bounded wait sliced so the deadline is always honored. */
void boundedSleep(uint64_t untilMs, uint64_t deadlineMs) {
  for (;;) {
    const uint64_t now = steadyNowMs();
    const uint64_t end = untilMs < deadlineMs ? untilMs : deadlineMs;
    if (now >= end || now >= deadlineMs) {
      return;
    }
    uint64_t slice = end - now;
    if (slice > 10) {
      slice = 10;
    }
    usleep(static_cast<useconds_t>(slice * 1000));
  }
}

/* ---- request construction (shared by all phases) ---- */

void fillCommonRequest(hipObjTransferReqV2_t& req, const char* method,
                       const char* bucket, const char* key, uint64_t size,
                       uint64_t offset, const char* query, uint32_t cookie,
                       uint32_t clientPsn, uint64_t deadlineMs,
                       uint64_t remainingMs) {
  std::memset(&req, 0, sizeof(req));
  req.method = method;
  req.bucket = bucket;
  req.key = key;
  req.query = query;
  req.size = size;
  req.offset = offset;
  req.cookie = cookie;
  req.clientPsn = clientPsn;
  req.deadlineMs = static_cast<uint32_t>(
    deadlineMs > 0xffffffff ? 0xffffffff : deadlineMs);
  req.remainingMs = static_cast<uint32_t>(
    remainingMs > 0xffffffff ? 0xffffffff : remainingMs);
}

struct SessionResources {
  DeviceHandle* dh = nullptr;
  RcConnV2 conn;
  ConnId id = 0;
  bool inserted = false;
  void* pinnedBuffer = nullptr;
};

/* Creates the per-transfer QP/CQ pair as a registry entry with a
 * retired-ring reservation, and pins the buffer MR. Returns 0 on
 * success; on failure every partial resource is rolled back. */
int acquireSession(void* devPtr, SessionResources& res) {
  ConnectionRegistry& reg = registry();
  if (!g_bufferMap.acquireMrRef(devPtr)) {
    return -1;
  }
  res.pinnedBuffer = devPtr;
  if (!reg.reserveSlot()) {
    return -1;
  }
  bool rollbackFailed = false;
  const int cret = createRcConnV2(v2State().device, res.conn,
                                  &rollbackFailed);
  if (cret != 0) {
    reg.unreserveSlot();
    return rollbackFailed ? -2 : -1;
  }
  ConnectionEntryV2 entry;
  entry.conn = res.conn;
  entry.device = v2State().device;
  entry.clientPsn = 0;
  res.id = reg.insert(std::move(entry));
  if (res.id == 0) {
    bool qpOk = false;
    bool cqOk = false;
    destroyRcConnV2(res.conn, &qpOk, &cqOk);
    releaseDevice(v2State().device);
    reg.unreserveSlot();
    return -1;
  }
  res.dh = v2State().device;
  res.inserted = true;
  if (transitionQpToInitV2(res.dh, res.conn) != 0) {
    return -1;
  }
  return 0;
}

/* Releases the session entry. Returns the release outcome so callers
 * can map Busy/leftover per the documented policy. */
int releaseSession(SessionResources& res) {
  if (res.pinnedBuffer != nullptr) {
    /* The MR pin is held until the release fully succeeds; a
     * Busy/leftover outcome keeps the entry (and its pin) alive. */
    int rc = 0;
    if (!res.inserted) {
      rc = 0;
      bool qpOk = false;
      bool cqOk = false;
      destroyRcConnV2(res.conn, &qpOk, &cqOk);
      releaseDevice(v2State().device);
      if (!qpOk || !cqOk) {
        rc = kReleaseLeftover;
      }
    } else {
      rc = releaseConnection(res.id);
    }
    if (rc == kReleaseOk && res.pinnedBuffer != nullptr) {
      g_bufferMap.releaseMrRef(res.pinnedBuffer);
      res.pinnedBuffer = nullptr;
    }
    return rc;
  }
  return kReleaseOk;
}

/* Maps an internal failure code to the public error with the
 * non-quiesced-release policy applied. releaseRc is the outcome of
 * releaseSession(); when it failed, the entry stays owned by the
 * registry and the buffer stays pinned, so the transfer surfaces
 * hipObjInternalError regardless of the wire outcome. */
hipObjError_t finalizeOutcome(hipObjError_t wireOutcome, int releaseRc) {
  if (releaseRc == kReleaseOk) {
    return wireOutcome;
  }
  return {hipObjInternalError, 0};
}

} // namespace

int v2Init(hipObjConfigV2_t* config) {
  V2State& st = v2State();
  if (st.initialized) {
    return hipObjAlreadyInitialized;
  }
  if (hipObj::getState().initialized) {
    return hipObjAlreadyInitialized;
  }
  if (!config || !config->control.controlEndpoint ||
      config->control.controlEndpoint[0] == '\0') {
    return hipObjInvalidValue;
  }
  if (!ibv.is_initialized) {
    return hipObjRdmaError;
  }
  /* gpuDevice < 0 (auto-select) was resolved by hipObjInitV2 before
   * calling v2Init; a negative value here means the lookup failed. */
  const int gpuDevice = config->v1.gpuDevice;
  if (gpuDevice < 0) {
    return hipObjRdmaError;
  }
  const char* devName = nullptr;
  int nicIndex = hipObj::GetClosestNicToGpu(
    gpuDevice, config->v1.nicHint ? config->v1.nicHint : nullptr, &devName);
  if (nicIndex < 0) {
    if (config->v1.nicHint && config->v1.nicHint[0] != '\0') {
      devName = config->v1.nicHint;
      nicIndex = 0;
    } else {
      return hipObjNicNotFound;
    }
  }
  DeviceHandle* dh = new DeviceHandle();
  RcConnection raw;
  const int ret = (devName != nullptr)
                    ? hipObj::openRdmaDeviceByName(devName, raw)
                    : hipObj::openRdmaDevice(nicIndex, raw);
  if (ret != 0) {
    delete dh;
    return hipObjRdmaError;
  }
  dh->ctx = raw.ctx;
  dh->pd = raw.pd;
  dh->portNum = raw.portNum;
  dh->gidIndex = raw.gidIndex;
  dh->localGid = raw.localGid;
  st.device = dh;
  st.controlEndpoint = config->control.controlEndpoint;
  st.connectDeadlineMs = config->connectDeadlineMs;
  st.transferDeadlineMs = config->transferDeadlineMs;
  st.cancelBudgetMs = config->cancelCleanupBudgetMs;
  st.initialized = true;
  return hipObjSuccess;
}

int v2Shutdown() {
  V2State& st = v2State();
  if (!st.initialized) {
    return hipObjSuccess;
  }
  ConnectionRegistry& reg = registry();
  bool poisonLeft = false;
  std::vector<ConnId> ids;
  reg.forEachId([&ids](ConnId id) { ids.push_back(id); });
  for (auto id : ids) {
    if (releaseConnection(id) == kReleaseLeftover) {
      poisonLeft = true;
    }
  }
  if (poisonLeft || reg.size() > 0) {
    return hipObjRdmaError;
  }
  if (st.device != nullptr) {
    RcConnection raw;
    raw.ctx = st.device->ctx;
    raw.pd = st.device->pd;
    raw.portNum = st.device->portNum;
    raw.gidIndex = st.device->gidIndex;
    raw.localGid = st.device->localGid;
    hipObj::closeRdmaDevice(raw);
    delete st.device;
    st.device = nullptr;
  }
  st.initialized = false;
  st.controlEndpoint.clear();
  return hipObjSuccess;
}

int v2Transfer(int isPut, const char* bucket, const char* key, void* devPtr,
               uint64_t size, uint64_t offset, const char* query,
               hipObjOpsV2_t* ops, void* ctx) {
  V2State& st = v2State();
  if (!st.initialized) {
    return hipObjNotInitialized;
  }
  if (!ops || !ops->sendPrepare || !ops->sendReadyRequest ||
      !ops->finishReady || !ops->sendCancel || !bucket || !key || !devPtr) {
    return hipObjInvalidValue;
  }
  if (size == 0 || size > kMaxTransferSize) {
    return hipObjSizeTooLarge;
  }
  struct ibv_mr* mr = g_bufferMap.lookupMr(devPtr);
  if (mr == nullptr) {
    return hipObjBufNotRegistered;
  }
  if (g_bufferMap.lookupSize(devPtr) < size) {
    return hipObjSizeTooLarge;
  }

  /* Whole-transfer budget: entry to return, lock/admission wait
   * included (the caller already holds apiLock via the entry point).
   * The CQ deadline is derived from this single budget. */
  const uint64_t startMs = steadyNowMs();
  const uint32_t budgetMs = st.transferDeadlineMs != 0
                              ? st.transferDeadlineMs
                              : kDefaultTransferDeadlineMs;
  const uint64_t deadline = startMs + budgetMs;
  const uint32_t cancelBudgetMs = st.cancelBudgetMs != 0
                                    ? st.cancelBudgetMs
                                    : kDefaultCancelBudgetMs;

  hipObjError_t outcome = {hipObjInternalError, 0};
  SessionResources res;
  Phase p = Phase::Idle;
  bool wireCancelEligible = false; /* session published on the wire */
  std::string sessionId;

  const auto cleanup = [&](hipObjError_t wireOutcome) -> int {
    fail(p, !exposed(p));
    /* Post-expiry cancel: one bounded attempt once a session was
     * published. PREPARE-phase expiry has nothing to cancel (the
     * parser requires a 32-hex session). */
    if (wireCancelEligible && !sessionId.empty()) {
      hipObjTransferReqV2_t creq;
      fillCommonRequest(creq, isPut ? "PUT" : "GET", bucket, key, size,
                        offset, query, 0, 0, deadline,
                        cancelBudgetMs);
      creq.session = sessionId.c_str();
      creq.target = nullptr;
      ops->sendCancel(ctx, &creq);
    }
    const int rc = releaseSession(res);
    const hipObjError_t fin0 = finalizeOutcome(wireOutcome, rc);
    return fin0.opError;
  };

  do {
    if (!beginNegotiate(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }

    /* ---- verbs setup (per-transfer QP/CQ, MR pin) ---- */
    if (acquireSession(devPtr, res) != 0) {
      outcome = {hipObjRdmaError, 0};
      /* acquireSession rolled its own resources back; releaseSession
       * below still drops the MR pin when nothing was inserted. */
      res.inserted = false;
      res.conn = RcConnV2{};
      break;
    }
    uint32_t psn = 1;
    if (!nextClientPsn(psn)) {
      psn = 1;
    }
    res.conn.qpNum = res.conn.qp != nullptr ? res.conn.qp->qp_num : 0;
    {
      ConnectionRegistry& reg = registry();
      reg.withEntry(res.id, [&](ConnectionEntryV2& entry) {
        entry.clientPsn = psn;
      });
    }

    /* Client token carries this QP's identity for the server's RTR. */
    hipObj::RdmaToken tok{};
    tok.qpNum = res.conn.qpNum;
    std::memcpy(tok.gid, &res.dh->localGid, sizeof(tok.gid));
    tok.transport = hipObj::TRANSPORT_RC;
    tok.portNum = res.dh->portNum;
    const std::string clientToken = hipObj::encodeRdmaToken(tok);

    /* ---- PREPARE ---- */
    std::string target = buildTarget(bucket, key,
                                     query != nullptr ? query : "");
    uint32_t cookie = 0;
    if (!nextCookie(cookie)) {
      cookie = 0x1a2b3c4d;
    }
    hipObjTransferReqV2_t preq;
    fillCommonRequest(preq, isPut ? "PUT" : "GET", bucket, key, size, offset,
                      query, cookie, psn, deadline, remaining(deadline));
    preq.token = clientToken.c_str();
    preq.target = target.c_str();
    preq.endpoint = nullptr; /* filled by the caller's endpoint */

    hipObjPrepareReplyV2_t prep;
    std::memset(&prep, 0, sizeof(prep));
    const int prc = ops->sendPrepare(ctx, &preq, &prep);
    if (prc != 0) {
      outcome = remaining(deadline) == 0 ? hipObjError_t{hipObjBusy, 0}
                                         : hipObjError_t{hipObjS3Error, 0};
      break;
    }
    if (prep.httpStatus == 501 && prep.unsupportedMarker) {
      outcome = {hipObjNotSupported, 0};
      break;
    }
    if (prep.httpStatus != 200) {
      outcome = prep.httpStatus == 503
                  ? hipObjError_t{hipObjBusy, 0}
                  : (prep.httpStatus == 403 || prep.httpStatus == 400
                       ? hipObjError_t{hipObjInvalidValue, 0}
                       : hipObjError_t{hipObjS3Error, 0});
      break;
    }
    if (!prepareOk(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    /* Validate the reply token: the peer transport must decode, match
     * the advertised QPN, and carry a routable GID. */
    hipObj::RdmaToken replyTok{};
    bool peerOk = false;
    uint32_t serverQpn = 0;
    union ibv_gid serverGid{};
    {
      /* serverToken is the 88-hex payload; the "200:" prefix was
       * stripped by the consumer (bridge/Go wrapper) per the wire. */
      if (prep.serverToken[0] != '\0' &&
          hipObj::decodeRdmaTokenHex(prep.serverToken, replyTok) &&
          replyTok.transport == hipObj::TRANSPORT_RC) {
        static const uint8_t kZeroGid[16] = {0};
        if (std::memcmp(replyTok.gid, kZeroGid, 16) != 0) {
          serverQpn = replyTok.qpNum;
          std::memcpy(&serverGid, replyTok.gid, 16);
          peerOk = true;
        }
      }
    }
    if (!peerOk || prep.session[0] == '\0') {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    sessionId = prep.session;
    wireCancelEligible = true;
    const uint32_t serverPsn =
      prep.serverPsn != 0 ? prep.serverPsn : 1;
    /* PUT requires a valid staging advertisement. */
    if (isPut && (!prep.stagingPresent || prep.stagingAddr == 0)) {
      outcome = {hipObjInvalidValue, 0};
      break;
    }

    /* ---- QP RTR/RTS ---- */
    if (transitionQpToRtrV2(res.dh, res.conn, serverQpn, 0, serverGid,
                            serverPsn) != 0 ||
        transitionQpToRtsV2(res.conn, res.dh, psn) != 0) {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    if (!connectOk(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }

    /* ---- READY request (bytes out; response stays pending) ---- */
    if (!sendReady(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    hipObjTransferReqV2_t rreq;
    fillCommonRequest(rreq, isPut ? "PUT" : "GET", bucket, key, size, offset,
                      query, cookie, psn, deadline, remaining(deadline));
    rreq.session = sessionId.c_str();
    rreq.token = clientToken.c_str();
    rreq.clientQpn = res.conn.qpNum;
    rreq.clientMrAddr = reinterpret_cast<uint64_t>(devPtr);
    rreq.clientMrRkey = mr->rkey;
    if (ops->sendReadyRequest(ctx, &rreq) != 0) {
      outcome = {hipObjS3Error, 0};
      break;
    }

    /* ---- data phase while the exchange is pending ---- */
    bool dataOk = false;
    struct ibv_wc wc = {};
    if (!isPut) {
      /* GET: post the registered buffer as the receive before the
       * server's WRITE lands, then poll RECV_RDMA_WITH_IMM. */
      struct ibv_sge sge = {};
      sge.addr = reinterpret_cast<uintptr_t>(devPtr);
      sge.length = static_cast<uint32_t>(size);
      sge.lkey = mr->lkey;
      struct ibv_recv_wr wr = {};
      wr.sg_list = &sge;
      wr.num_sge = 1;
      struct ibv_recv_wr* bad = nullptr;
      if (ibv.post_recv(res.conn.qp, &wr, &bad) == 0) {
        dataOk = pollDeadline(res.conn.cq, IBV_WC_RECV_RDMA_WITH_IMM,
                              deadline, wc) &&
                 (wc.wc_flags & IBV_WC_WITH_IMM) != 0 &&
                 ntohl(wc.imm_data) == cookie;
      }
    } else {
      /* PUT staging push: bounded grace so the server's receive post
       * (inside its READY handling) is not raced, then WRITE WITH
       * IMM to the staging MR. */
      boundedSleep(steadyNowMs() + kPutGraceMs, deadline);
      if (steadyNowMs() >= deadline) {
        break;
      }
      struct ibv_sge sge = {};
      sge.addr = reinterpret_cast<uintptr_t>(devPtr);
      sge.length = static_cast<uint32_t>(size);
      sge.lkey = mr->lkey;
      struct ibv_send_wr wr = {};
      wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      wr.send_flags = IBV_SEND_SIGNALED;
      wr.imm_data = htonl(cookie);
      wr.wr.rdma.remote_addr = prep.stagingAddr;
      wr.wr.rdma.rkey = prep.stagingRkey;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      struct ibv_send_wr* bad = nullptr;
      if (ibv.post_send(res.conn.qp, &wr, &bad) == 0) {
        dataOk = pollDeadline(res.conn.cq, IBV_WC_RDMA_WRITE, deadline, wc);
      }
    }
    if (!dataOk) {
      outcome = remaining(deadline) == 0 ? hipObjError_t{hipObjBusy, 0}
                                         : hipObjError_t{hipObjRdmaError, 0};
      break;
    }

    /* ---- finishReady: consume the pending FINAL response ---- */
    hipObjFinalReplyV2_t fin;
    std::memset(&fin, 0, sizeof(fin));
    if (ops->finishReady(ctx, &rreq, &fin) != 0) {
      outcome = {hipObjS3Error, 0};
      break;
    }
    if (fin.httpStatus != (isPut ? 204 : 200)) {
      outcome = {hipObjS3Error, 0};
      break;
    }
    if (!fin.cookiePresent || fin.cookieEcho != cookie) {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    if (!transferDone(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    outcome = HIPOBJ_SUCCESS;
  } while (false);

  return cleanup(outcome);
}

} // namespace v2
} // namespace hipObj
