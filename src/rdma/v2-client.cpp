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
 * Everything here runs under ApiGuard with no worker threads:
 * helpers join before the entry point returns.
 */

#include "v2-client.h"

#include <optional>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <thread>

#include "ibv-gid.h"
#include "ibv-ptr.h"
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
/* hipObjError_t has no timeout code. Busy plus this marker records
 * that the whole-transfer deadline expired. */
constexpr int kDiagDeadlineExpired = 0x54494D45; /* "TIME" */
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

/* ---- per-process v2 state (guarded by ApiGuard) ---- */

struct V2State {
  bool initialized = false;
  std::string controlEndpoint;
  uint32_t connectDeadlineMs = 0;
  uint32_t transferDeadlineMs = 0;
  uint32_t cancelBudgetMs = 0;
  /* RDMA device name selected at init. v2 callers query it instead of
   * minting a v1 token just to learn the NIC. */
  std::string nicName;
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
    const int n = ibv().poll_cq(cq, 1, &cur);
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
  /* deadlineMs is the remaining whole-transfer budget per the header
   * contract, not the absolute steady-clock deadline. The absolute
   * deadline stays a caller-side bound and is not copied into the request. */
  (void)deadlineMs;
  req.deadlineMs = static_cast<uint32_t>(
    remainingMs > 0xffffffff ? 0xffffffff : remainingMs);
  req.remainingMs = req.deadlineMs;
}

struct SessionResources {
  DeviceHandle* dh = nullptr;
  RcConnV2 conn;
  struct ibv_qp* qp = nullptr;
  struct ibv_cq* cq = nullptr;
  uint32_t qpNum = 0;
  ConnId id = 0;
  bool inserted = false;
  bool deviceRefHeld = false;
  void* pinnedBuffer = nullptr;
};

/* Creates the per-transfer QP/CQ pair as a registry entry with a
 * retired-ring reservation, and pins the buffer MR. Returns 0 on
 * success; on failure every partial resource is rolled back. */
int acquireSession(void* devPtr, SessionResources& res) {
  ConnectionRegistry& reg = registry();
  /* Expired (qpn, psn) records are reclaimable. Collect them before
   * reserving so a full ring does not turn every teardown Busy. */
  reg.retired().collectExpired(std::chrono::steady_clock::now());
  if (!bufferMap().acquireMrRef(devPtr)) {
    return -1;
  }
  res.pinnedBuffer = devPtr;
  if (!reg.reserveSlot()) {
    bufferMap().releaseMrRef(devPtr);
    res.pinnedBuffer = nullptr;
    return -1;
  }
  /* Reserve retirement capacity before the QP exists. The pair is
   * published only once a slot is held, so teardown cannot block on
   * an exhausted ring. */
  RetiredRing::Reservation retire(reg.retired());
  if (!retire) {
    bufferMap().releaseMrRef(devPtr);
    res.pinnedBuffer = nullptr;
    reg.unreserveSlot();
    return -1;
  }
  const int cret = createRcConnV2(*v2State().device, res.conn);
  if (cret != 0) {
    /* Refs are not taken on failure. A cq that would not destroy must
     * not be destroyed again when this local goes away. The retirement
     * reservation unreserves when retire leaves this scope. */
    const bool leftover = static_cast<bool>(res.conn.cq);
    if (leftover) {
      hipObj::dropOwnership(res.conn.cq);
    }
    reg.unreserveSlot();
    return leftover ? -2 : -1;
  }
  res.deviceRefHeld = true;
  res.dh = v2State().device;
  if (transitionQpToInitV2(*res.dh, res.conn) != 0) {
    const DestroyResult destroyed = destroyRcConnV2(res.conn);
    if (destroyed.qpOk && destroyed.cqOk) {
      releaseDevice(*res.dh);
      res.deviceRefHeld = false;
    } else {
      hipObj::dropOwnership(res.conn.qp);
      hipObj::dropOwnership(res.conn.cq);
    }
    reg.unreserveSlot();
    return (destroyed.qpOk && destroyed.cqOk) ? -1 : -2;
  }
  res.qp = res.conn.qp.get();
  res.cq = res.conn.cq.get();
  res.qpNum = res.conn.qpNum;
  ConnectionEntryV2 entry;
  entry.conn = std::move(res.conn);
  entry.device = res.dh;
  entry.clientPsn = 0;
  entry.reservationId = retire.release();
  res.id = reg.insert(std::move(entry));
  if (res.id == 0) {
    /* insert() returns before it moves when the reserve is gone, so
     * this local still owns the connection and the retirement id. */
    reg.retired().unreserve(entry.reservationId);
    entry.reservationId = 0;
    res.qp = nullptr;
    res.cq = nullptr;
    releaseDevice(*res.dh);
    res.deviceRefHeld = false;
    reg.unreserveSlot();
    return -1;
  }
  res.inserted = true;
  return 0;
}

/* Releases the session entry. Returns the release outcome so callers
 * can map Busy/leftover per the documented policy. */
ReleaseResult releaseSession(SessionResources& res) {
  /* The MR pin is held until the release fully succeeds; a
   * Busy/leftover outcome keeps the entry (and its pin) alive. */
  ReleaseResult rc = ReleaseResult::Ok;
  if (res.inserted) {
    rc = releaseConnection(res.id);
    if (rc == ReleaseResult::Ok && res.deviceRefHeld &&
        v2State().device != nullptr) {
      releaseDevice(*v2State().device);
      res.deviceRefHeld = false;
    }
  } else if (res.conn.qp || res.conn.cq) {
    const DestroyResult destroyed = destroyRcConnV2(res.conn);
    if (!destroyed.qpOk || !destroyed.cqOk) {
      hipObj::dropOwnership(res.conn.qp);
      hipObj::dropOwnership(res.conn.cq);
      rc = ReleaseResult::Leftover;
    } else if (res.deviceRefHeld && v2State().device != nullptr) {
      releaseDevice(*v2State().device);
      res.deviceRefHeld = false;
    }
  } else if (res.deviceRefHeld && v2State().device != nullptr) {
    releaseDevice(*v2State().device);
    res.deviceRefHeld = false;
  }
  if (rc == ReleaseResult::Ok && res.pinnedBuffer != nullptr) {
    bufferMap().releaseMrRef(res.pinnedBuffer);
    res.pinnedBuffer = nullptr;
  }
  return rc;
}
} // namespace

/* Maps an internal failure code to the public error with the
 * non-quiesced-release policy applied. releaseRc is the outcome of
 * releaseSession(); when it failed, the entry stays owned by the
 * registry and the buffer stays pinned, so the transfer surfaces
 * hipObjInternalError regardless of the wire outcome. */
hipObjError_t finalizeOutcome(hipObjError_t wireOutcome, ReleaseResult releaseRc) {
  if (releaseRc == ReleaseResult::Ok) {
    return wireOutcome;
  }
  return {hipObjInternalError, 0};
}

bool v2IsInitialized() { return v2State().initialized; }

const char* v2NicName() { return v2State().nicName.c_str(); }

bool v2IsInitialized() {
  return v2State().initialized;
}

const char* v2NicName() {
  return v2State().nicName.c_str();
}

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
  if (!hipObj::ibvAvailable()) {
    return hipObjRdmaError;
  }
  /* gpuDevice < 0 (auto-select) was resolved by hipObjInitV2 before
   * calling v2Init. -1 is allowed only with a NIC hint, matching
   * hipObjInit. */
  const int gpuDevice = config->v1.gpuDevice;
  const bool haveNicHint = config->v1.nicHint && config->v1.nicHint[0] != '\0';
  if (gpuDevice < -1) {
    return hipObjInvalidValue;
  }
  std::optional<hipObj::RdmaNic> nic;
  if (gpuDevice >= 0) {
    nic = hipObj::GetClosestNicToGpu(gpuDevice, config->v1.nicHint);
  }
  if (!nic) {
    if (haveNicHint) {
      nic = hipObj::RdmaNic{0, config->v1.nicHint};
    } else {
      return hipObjNicNotFound;
    }
  }
  DeviceHandle* dh = new DeviceHandle();
  RcConnection raw;
  if (hipObj::openRdmaDeviceByName(nic->name.c_str(), raw) != 0) {
    delete dh;
    return hipObjRdmaError;
  }
  dh->ctx = std::move(raw.ctx);
  dh->pd = std::move(raw.pd);
  dh->portNum = raw.portNum;
  dh->gidIndex = raw.gidIndex;
  dh->localGid = raw.localGid;
  st.device = dh;
  st.controlEndpoint = config->control.controlEndpoint;
  st.connectDeadlineMs = config->connectDeadlineMs;
  st.transferDeadlineMs = config->transferDeadlineMs;
  st.cancelBudgetMs = config->cancelCleanupBudgetMs;
  st.nicName = nic->name;
  st.initialized = true;
  return hipObjSuccess;
}

struct ibv_pd *v2ProtectionDomain()
{
  V2State &st = v2State();
  if (!st.initialized || st.device == nullptr) {
    return nullptr;
  }
  return st.device->pd.get();
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
    const ReleaseResult rc = releaseConnection(id);
    if (rc == ReleaseResult::Ok) {
      if (st.device != nullptr) {
        releaseDevice(*st.device);
      }
    } else if (rc == ReleaseResult::Leftover) {
      poisonLeft = true;
    }
  }
  if (poisonLeft || reg.size() > 0) {
    return hipObjRdmaError;
  }
  /* Buffers go before the device: every MR must be deregistered
   * while the protection domain is still alive. */
  bufferMap().deregisterAll();
  if (st.device != nullptr) {
    RcConnection raw;
    raw.ctx = std::move(st.device->ctx);
    raw.pd = std::move(st.device->pd);
    raw.portNum = st.device->portNum;
    raw.gidIndex = st.device->gidIndex;
    raw.localGid = st.device->localGid;
    hipObj::closeRdmaDevice(raw);
    delete st.device;
    st.device = nullptr;
  }
  st.initialized = false;
  st.controlEndpoint.clear();
  st.nicName.clear();
  return hipObjSuccess;
}

int v2Transfer(int isPut, const char* bucket, const char* key, void* devPtr,
               uint64_t size, uint64_t offset, const char* query,
               hipObjOpsV2_t* ops, void* ctx, uint64_t entryMs) {
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
  const auto* buf = bufferMap().find(devPtr);
  if (buf == nullptr || !buf->mr) {
    return hipObjBufNotRegistered;
  }
  if (!bufferMap().isDeviceBacked(devPtr)) {
    /* A host bounce buffer covers different memory than the advertised
     * device pointer. v2 must not DMA through it. */
    return hipObjBufNotRegistered;
  }
  if (buf->size < size) {
    return hipObjSizeTooLarge;
  }
  struct ibv_mr* mr = buf->mr.get();

  /* Whole-transfer budget: entry to return, lock/admission wait
   * included (the caller already holds ApiGuard via the entry point).
   * The CQ deadline is derived from this single budget. */
  const uint64_t startMs = entryMs;
  const uint32_t budgetMs = st.transferDeadlineMs != 0
                              ? st.transferDeadlineMs
                              : kDefaultTransferDeadlineMs;
  const uint64_t deadline = startMs + budgetMs;
  if (steadyNowMs() >= deadline) {
    return hipObjError_t{hipObjBusy, kDiagDeadlineExpired}.opError;
  }
  const uint32_t cancelBudgetMs = st.cancelBudgetMs != 0
                                    ? st.cancelBudgetMs
                                    : kDefaultCancelBudgetMs;
  /* The stored endpoint string outlives every callback of this init. */
  hipObjControlEndpointV2_t epV2{st.controlEndpoint.c_str()};

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
    if (wireCancelEligible && !sessionId.empty() &&
        wireOutcome.opError != hipObjSuccess) {
      hipObjTransferReqV2_t creq;
      fillCommonRequest(creq, isPut ? "PUT" : "GET", bucket, key, size,
                        offset, query, 0, 0, deadline,
                        cancelBudgetMs);
      creq.session = sessionId.c_str();
      creq.target = nullptr;
      creq.endpoint = &epV2;
      ops->sendCancel(ctx, &creq);
    }
    const ReleaseResult rc = releaseSession(res);
    const hipObjError_t fin0 = finalizeOutcome(wireOutcome, rc);
    return fin0.opError;
  };

  do {
    if (!beginNegotiate(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }

    /* ---- verbs setup (per-transfer QP/CQ, MR pin) ---- */
    const int arc = acquireSession(devPtr, res);
    if (arc != 0) {
      /* acquireSession rolls its own QP work back, but a failed destroy
       * (arc == -2) leaves a dropped cq that releaseSession must not
       * destroy again. Do not clear res: the pin is still in
       * pinnedBuffer when the failure happened after the ref was taken. */
      outcome = arc == -2 ? hipObjError_t{hipObjInternalError, 0}
                          : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    const auto psnOpt = nextClientPsn();
    if (!psnOpt || *psnOpt == 0) {
      /* A missing random source must not reuse a fixed PSN. */
      outcome = {hipObjInternalError, 0};
      break;
    }
    const uint32_t psn = *psnOpt;
    {
      ConnectionRegistry& reg = registry();
      reg.withEntry(res.id, [&](ConnectionEntryV2& entry) {
        entry.clientPsn = psn;
      });
    }

    /* Client token carries this QP's identity for the server's RTR. */
    hipObj::RdmaToken tok{};
    tok.qpNum = res.qpNum;
    tok.gid = hipObj::toGid(res.dh->localGid);
    tok.transport = hipObj::TransportType::RC;
    tok.portNum = res.dh->portNum;
    const std::string clientToken = hipObj::encodeRdmaToken(tok);

    /* ---- PREPARE ---- */
    std::string target = buildTarget(bucket, key,
                                     query != nullptr ? query : "");
    const auto cookieOpt = nextCookie();
    if (!cookieOpt || *cookieOpt == 0) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    const uint32_t cookie = *cookieOpt;
    hipObjTransferReqV2_t preq;
    fillCommonRequest(preq, isPut ? "PUT" : "GET", bucket, key, size, offset,
                      query, cookie, psn, deadline, remaining(deadline));
    preq.token = clientToken.c_str();
    preq.target = target.c_str();
    preq.endpoint = &epV2;

    hipObjPrepareReplyV2_t prep;
    std::memset(&prep, 0, sizeof(prep));
    const int prc = ops->sendPrepare(ctx, &preq, &prep);
    if (prc != 0) {
      outcome = remaining(deadline) == 0
                    ? hipObjError_t{hipObjBusy, kDiagDeadlineExpired}
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
      const auto decoded = prep.serverToken[0] != '\0'
                               ? hipObj::decodeRdmaTokenHex(prep.serverToken)
                               : std::nullopt;
      if (decoded && decoded->transport == hipObj::TransportType::RC &&
          decoded->gid != hipObj::Gid{}) {
        replyTok = *decoded;
        serverQpn = replyTok.qpNum;
        serverGid = hipObj::toIbvGid(replyTok.gid);
        peerOk = true;
      }
    }
    const bool sessionHex = [&] {
      const char* q = prep.session;
      for (size_t i = 0; i < 32; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(q[i]))) {
          return false;
        }
      }
      return q[32] == '\0';
    }();
    if (!peerOk || !sessionHex || !prep.protocolEcho) {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    if (prep.serverPsn == 0 || prep.serverPsn > 0xffffffu) {
      /* The wire contract requires a nonzero 24-bit server PSN. */
      outcome = {hipObjRdmaError, 0};
      break;
    }
    sessionId = prep.session;
    wireCancelEligible = true;
    const uint32_t serverPsn = prep.serverPsn;
    /* PUT requires a valid staging advertisement. */
    if (isPut && (!prep.stagingPresent || prep.stagingAddr == 0)) {
      outcome = {hipObjInvalidValue, 0};
      break;
    }

    /* ---- QP RTR/RTS ---- */
    bool verbsOk = false;
    registry().withEntry(res.id, [&](ConnectionEntryV2& entry) {
      verbsOk = transitionQpToRtrV2(*res.dh, entry.conn, serverQpn, serverGid, serverPsn) == 0 &&
                transitionQpToRtsV2(entry.conn, *res.dh, psn) == 0;
    });
    if (!verbsOk) {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    if (!connectOk(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }

    /* GET receive goes up before READY. READY arms the server's
     * WRITE_WITH_IMM, so a late post burns RNR retries. */
    struct ibv_recv_wr recvWr = {};
    struct ibv_sge recvSge = {};
    if (!isPut) {
      recvSge.addr = reinterpret_cast<uintptr_t>(devPtr);
      recvSge.length = static_cast<uint32_t>(size);
      recvSge.lkey = mr->lkey;
      recvWr.sg_list = &recvSge;
      recvWr.num_sge = 1;
      struct ibv_recv_wr* bad = nullptr;
      if (ibv().post_recv(res.qp, &recvWr, &bad) != 0) {
        outcome = {hipObjRdmaError, 0};
        break;
      }
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
    rreq.endpoint = &epV2;
    rreq.clientQpn = res.qpNum;
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
      /* GET: the receive was posted before READY. Poll the server's
       * WRITE_WITH_IMM completion now. */
      dataOk = pollDeadline(res.cq, IBV_WC_RECV_RDMA_WITH_IMM,
                            deadline, wc) &&
               (wc.wc_flags & IBV_WC_WITH_IMM) != 0 &&
               ntohl(wc.imm_data) == cookie;
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
      if (ibv().post_send(res.qp, &wr, &bad) == 0) {
        dataOk = pollDeadline(res.cq, IBV_WC_RDMA_WRITE, deadline, wc);
      }
    }
    if (!dataOk) {
      outcome = remaining(deadline) == 0
                    ? hipObjError_t{hipObjBusy, kDiagDeadlineExpired}
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
    const bool finalOk = fin.httpStatus == 200 ||
                         (isPut && fin.httpStatus == 204);
    if (!finalOk) {
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
    outcome = {hipObjSuccess, 0};
  } while (false);

  return cleanup(outcome);
}

} // namespace v2
} // namespace hipObj
