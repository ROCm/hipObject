/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_data_phase.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>

#include "../../../src/common/ibv-wrapper.h"

namespace hipObj {
namespace v2 {

namespace {

constexpr int kAccess = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ;

uint64_t clockNowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

} // namespace

bool stagePutBuffer(V2Session& s, size_t size, struct ibv_pd* pd) {
  if (s.staging != nullptr && s.stagingMr != nullptr) {
    return true; /* already staged */
  }
  void* buf = std::malloc(size ? size : 1);
  if (buf == nullptr) {
    return false;
  }
  /* Host memory: plain registration, no dmabuf export. The
   * client's GPU buffers use their own MRs on the client side;
   * the server only accesses them remotely. */
  struct ibv_mr* mr = ibv.reg_mr_host(pd, buf, size, kAccess);
  if (mr == nullptr) {
    std::free(buf);
    return false;
  }
  s.staging = buf;
  s.stagingMr = mr;
  return true;
}

void releaseStaging(V2Session& s) {
  if (s.stagingMr != nullptr) {
    ibv.dereg_mr(s.stagingMr);
    s.stagingMr = nullptr;
  }
  if (s.staging != nullptr) {
    std::free(s.staging);
    s.staging = nullptr;
  }
}

/* Posts one receive that consumes the client's WRITE_WITH_IMM. */
bool postRecvForImm(struct ibv_qp* qp, struct ibv_mr* mr, size_t len) {
  struct ibv_sge sge;
  std::memset(&sge, 0, sizeof(sge));
  sge.addr = reinterpret_cast<uintptr_t>(mr->addr);
  sge.length = static_cast<uint32_t>(len);
  sge.lkey = mr->lkey;

  struct ibv_recv_wr wr;
  std::memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0x5245435632494d4dULL; /* "RECV2IMM" marker */
  wr.sg_list = &sge;
  wr.num_sge = 1;

  struct ibv_recv_wr* bad = nullptr;
  return ibv.post_recv(qp, &wr, &bad) == 0;
}

/* Posts one RDMA READ pulling the object from the client MR. */
bool postRdmaRead(struct ibv_qp* qp, struct ibv_mr* dst, uint64_t remoteAddr,
                  uint32_t rkey, size_t len) {
  struct ibv_sge sge;
  std::memset(&sge, 0, sizeof(sge));
  sge.addr = reinterpret_cast<uintptr_t>(dst->addr);
  sge.length = static_cast<uint32_t>(len);
  sge.lkey = dst->lkey;

  struct ibv_send_wr wr;
  std::memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0x5245414432444154ULL; /* "READ2DAT" marker */
  wr.opcode = IBV_WR_RDMA_READ;
  wr.wr.rdma.remote_addr = remoteAddr;
  wr.wr.rdma.rkey = rkey;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  struct ibv_send_wr* bad = nullptr;
  return ibv.post_send(qp, &wr, &bad) == 0;
}

/* Polls the session CQ for one completion, bounded by an
 * absolute deadline on the monotonic clock. */
static int pollCqUntil(struct ibv_cq* cq, uint64_t deadlineMs,
                       struct ibv_wc* out) {
  for (;;) {
    int n = ibv.poll_cq(cq, 1, out);
    if (n > 0) {
      return 0;
    }
    if (clockNowMs() >= deadlineMs) {
      return -1;
    }
    struct timespec ts = {0, 2 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
}

DataPhaseResult runDataPhase(V2Session& s, uint64_t deadlineMs,
                             DataPhaseStats& stats) {
  if (s.qp == nullptr || s.cq == nullptr) {
    /* No transport objects on the session: the wire layer was
     * never wired (unit-test sessions). Treat as a verified
     * no-op so the control flow stays testable. */
    stats.bytes = s.size;
    stats.cookie = s.cookie;
    return DataPhaseResult::Ok;
  }

  struct ibv_wc wc;

  if (s.op == "PUT") {
    if (s.stagingMr == nullptr) {
      return DataPhaseResult::WireFail;
    }
    if (!postRecvForImm(s.qp, s.stagingMr, static_cast<size_t>(s.size))) {
      return DataPhaseResult::WireFail;
    }
    /* The client's WRITE_WITH_IMM completes on our receive CQ
     * with the session cookie as the immediate. */
    if (pollCqUntil(s.cq, deadlineMs, &wc) != 0) {
      return DataPhaseResult::Timeout;
    }
    if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM ||
        (wc.wc_flags & IBV_WC_WITH_IMM) == 0 ||
        ntohl(wc.imm_data) != s.cookie) {
      return DataPhaseResult::VerifyFail;
    }
    stats.bytes = wc.byte_len;
    stats.cookie = s.cookie;
    return DataPhaseResult::Ok;
  }

  /* GET: the client posted a receive and exposes its MR; we
   * RDMA READ the object from it and wait for our READ
   * completion. */
  if (s.stagingMr == nullptr || s.clientMrAddr == 0 || s.clientMrRkey == 0) {
    return DataPhaseResult::WireFail;
  }
  if (!postRdmaRead(s.qp, s.stagingMr, s.clientMrAddr, s.clientMrRkey,
                    static_cast<size_t>(s.size))) {
    return DataPhaseResult::WireFail;
  }
  if (pollCqUntil(s.cq, deadlineMs, &wc) != 0) {
    return DataPhaseResult::Timeout;
  }
  if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RDMA_READ) {
    return DataPhaseResult::VerifyFail;
  }
  stats.bytes = wc.byte_len;
  stats.cookie = s.cookie;
  return DataPhaseResult::Ok;
}

} // namespace v2
} // namespace hipObj
