/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "transport.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "ibv-wrapper.hpp"
#include "rdma-topology.hpp"
#include "token.hpp"
#include "vendor-ops.hpp"

namespace hipObj {

namespace {

constexpr int IBV_ACCESS_REMOTE_READ = 0x1;
constexpr int IBV_ACCESS_REMOTE_WRITE = 0x2;

} // namespace

int openRdmaDevice(int nicIndex, RcConnection& conn) {
  int numDevs = 0;
  struct ibv_device** devList = ibv.get_device_list(&numDevs);
  if (!devList || numDevs <= 0 || nicIndex >= numDevs) {
    return -1;
  }
  struct ibv_device* dev = devList[nicIndex];
  if (!dev) {
    ibv.free_device_list(devList);
    return -1;
  }
  conn.ctx = ibv.open_device(dev);
  ibv.free_device_list(devList);
  if (!conn.ctx) {
    return -1;
  }
  conn.pd = ibv.alloc_pd(conn.ctx);
  if (!conn.pd) {
    ibv.close_device(conn.ctx);
    conn.ctx = nullptr;
    return -1;
  }
  struct ibv_port_attr portAttr;
  if (ibv.query_port(conn.ctx, conn.portNum, &portAttr) != 0) {
    ibv.dealloc_pd(conn.pd);
    ibv.close_device(conn.ctx);
    conn.pd = nullptr;
    conn.ctx = nullptr;
    return -1;
  }
  conn.gidIndex = SelectBestGid(conn.ctx, conn.portNum);
  if (conn.gidIndex < 0) {
    conn.gidIndex = 0;
  }
  if (ibv.query_gid(conn.ctx, conn.portNum, conn.gidIndex, &conn.localGid) !=
      0) {
    ibv.dealloc_pd(conn.pd);
    ibv.close_device(conn.ctx);
    conn.pd = nullptr;
    conn.ctx = nullptr;
    return -1;
  }
  return 0;
}

int openRdmaDeviceByName(const char* devName, RcConnection& conn) {
  if (!devName) {
    return -1;
  }
  int numDevs = 0;
  struct ibv_device** devList = ibv.get_device_list(&numDevs);
  if (!devList || numDevs <= 0) {
    return -1;
  }
  int nicIndex = -1;
  for (int i = 0; i < numDevs; ++i) {
    if (ibv.get_device_name(devList[i]) &&
        std::strcmp(ibv.get_device_name(devList[i]), devName) == 0) {
      nicIndex = i;
      break;
    }
  }
  if (nicIndex < 0) {
    ibv.free_device_list(devList);
    return -1;
  }
  int ret = openRdmaDevice(nicIndex, conn);
  ibv.free_device_list(devList);
  return ret;
}

void closeRdmaDevice(RcConnection& conn) {
  if (conn.qp) {
    ibv.destroy_qp(conn.qp);
    conn.qp = nullptr;
  }
  if (conn.cq) {
    ibv.destroy_cq(conn.cq);
    conn.cq = nullptr;
  }
  if (conn.pd) {
    ibv.dealloc_pd(conn.pd);
    conn.pd = nullptr;
  }
  if (conn.ctx) {
    ibv.close_device(conn.ctx);
    conn.ctx = nullptr;
  }
}

int createRcQp(RcConnection& conn, int cqSize, int maxSendWr, int maxRecvWr) {
  conn.cq = ibv.create_cq(conn.ctx, cqSize, nullptr, nullptr, 0);
  if (!conn.cq) {
    return -1;
  }
  struct ibv_qp_init_attr initAttr;
  std::memset(&initAttr, 0, sizeof(initAttr));
  initAttr.send_cq = conn.cq;
  initAttr.recv_cq = conn.cq;
  initAttr.cap.max_send_wr = maxSendWr;
  initAttr.cap.max_recv_wr = maxRecvWr;
  initAttr.cap.max_send_sge = 1;
  initAttr.cap.max_recv_sge = 1;
  initAttr.qp_type = IBV_QPT_RC;
  conn.qp = ibv.create_qp(conn.pd, &initAttr);
  if (!conn.qp) {
    ibv.destroy_cq(conn.cq);
    conn.cq = nullptr;
    return -1;
  }
  return 0;
}

int transitionQpToInit(RcConnection& conn) {
  struct ibv_qp_attr attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = conn.portNum;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
             IBV_QP_ACCESS_FLAGS;
  return ibv.modify_qp(conn.qp, &attr, mask);
}

static void applyVendorQpAttrs(RcConnection& conn, struct ibv_qp_attr* attr) {
  if (!conn.ctx || !attr) {
    return;
  }
  struct ibv_device_attr devAttr;
  std::memset(&devAttr, 0, sizeof(devAttr));
  if (ibv.query_device(conn.ctx, &devAttr) != 0) {
    return;
  }
#ifdef HIPOBJ_BNXT
  if (isBnxtDevice(devAttr.vendor_id)) {
    configureBnxtQp(attr);
  }
#endif
#ifdef HIPOBJ_IONIC
  if (isIonicDevice(devAttr.vendor_id)) {
    configureIonicQp(attr);
  }
#endif
}

int transitionQpToRtr(RcConnection& conn, uint32_t destQpNum, uint16_t destLid,
                      union ibv_gid destGid) {
  struct ibv_qp_attr attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_4096;
  applyVendorQpAttrs(conn, &attr);
  attr.dest_qp_num = destQpNum;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;
  attr.ah_attr.is_global = 1;
  attr.ah_attr.dlid = destLid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = conn.portNum;
  attr.ah_attr.grh.dgid = destGid;
  attr.ah_attr.grh.flow_label = 0;
  attr.ah_attr.grh.hop_limit = 64;
  attr.ah_attr.grh.sgid_index = conn.gidIndex;
  attr.ah_attr.grh.traffic_class = 0;
  int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
             IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  return ibv.modify_qp(conn.qp, &attr, mask);
}

int transitionQpToRts(RcConnection& conn) {
  struct ibv_qp_attr attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  applyVendorQpAttrs(conn, &attr);
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
             IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  return ibv.modify_qp(conn.qp, &attr, mask);
}

int connectRcPeer(RcConnection& conn, const RdmaToken& peerToken) {
  if (!conn.qp || peerToken.transport != TRANSPORT_RC) {
    return -1;
  }
  union ibv_gid peerGid;
  std::memcpy(peerGid.raw, peerToken.gid, 16);
  int ret = transitionQpToRtr(conn, peerToken.qpNum, peerToken.lid, peerGid);
  if (ret != 0) {
    return ret;
  }
  return transitionQpToRts(conn);
}

int pollCompletion(RcConnection& conn, int expectedOpcode, int timeoutMs) {
  if (!conn.cq) {
    return -1;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    struct ibv_wc wc;
    int n = ibv.poll_cq(conn.cq, 1, &wc);
    if (n < 0) {
      return -1;
    }
    if (n == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (wc.status != IBV_WC_SUCCESS) {
      return -1;
    }
    if (expectedOpcode < 0 || wc.opcode == expectedOpcode) {
      return 0;
    }
  }
  return 0;
}

} // namespace hipObj
