/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "transport.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "rdma-topology.h"
#include "token.h"
#include "vendor-ops.h"

namespace hipObj {

namespace {

    constexpr int IBV_ACCESS_REMOTE_READ  = 0x1;
    constexpr int IBV_ACCESS_REMOTE_WRITE = 0x2;

} // namespace

int
openRdmaDevice(int nicIndex, RcConnection &conn)
{
    int              numDevs = 0;
    IbvDeviceListPtr devList(ibv.get_device_list(&numDevs));
    if (!devList || numDevs <= 0 || nicIndex < 0 || nicIndex >= numDevs) {
        return -1;
    }
    struct ibv_device *dev = devList[static_cast<size_t>(nicIndex)];
    if (!dev) {
        return -1;
    }
    IbvContextPtr ctx(ibv.open_device(dev));
    devList.reset();
    if (!ctx) {
        return -1;
    }
    IbvPdPtr pd(ibv.alloc_pd(ctx.get()));
    if (!pd) {
        return -1;
    }
    struct ibv_port_attr portAttr;
    if (ibv.query_port(ctx.get(), conn.portNum, &portAttr) != 0) {
        return -1;
    }
    conn.gidIndex = SelectBestGid(ctx.get(), conn.portNum);
    if (conn.gidIndex < 0) {
        conn.gidIndex = 0;
    }
    if (ibv.query_gid(ctx.get(), conn.portNum, conn.gidIndex, &conn.localGid) != 0) {
        return -1;
    }
    conn.pd  = std::move(pd);
    conn.ctx = std::move(ctx);
    return 0;
}

int
openRdmaDeviceByName(const char *devName, RcConnection &conn)
{
    if (!devName) {
        return -1;
    }
    int              numDevs = 0;
    IbvDeviceListPtr devList(ibv.get_device_list(&numDevs));
    if (!devList || numDevs <= 0) {
        return -1;
    }
    int nicIndex = -1;
    for (int i = 0; i < numDevs; ++i) {
        struct ibv_device *dev = devList[static_cast<size_t>(i)];
        if (ibv.get_device_name(dev) && std::strcmp(ibv.get_device_name(dev), devName) == 0) {
            nicIndex = i;
            break;
        }
    }
    if (nicIndex < 0) {
        return -1;
    }
    return openRdmaDevice(nicIndex, conn);
}

void
closeRdmaDevice(RcConnection &conn)
{
    conn.qp.reset();
    conn.cq.reset();
    conn.pd.reset();
    conn.ctx.reset();
}

int
createRcQp(RcConnection &conn, int cqSize, uint32_t maxSendWr, uint32_t maxRecvWr)
{
    IbvCqPtr cq(ibv.create_cq(conn.ctx.get(), cqSize, nullptr, nullptr, 0));
    if (!cq) {
        return -1;
    }
    struct ibv_qp_init_attr initAttr;
    std::memset(&initAttr, 0, sizeof(initAttr));
    initAttr.send_cq          = cq.get();
    initAttr.recv_cq          = cq.get();
    initAttr.cap.max_send_wr  = maxSendWr;
    initAttr.cap.max_recv_wr  = maxRecvWr;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;
    initAttr.qp_type          = IBV_QPT_RC;
    IbvQpPtr qp(ibv.create_qp(conn.pd.get(), &initAttr));
    if (!qp) {
        return -1;
    }
    conn.qp = std::move(qp);
    conn.cq = std::move(cq);
    return 0;
}

int
transitionQpToInit(RcConnection &conn)
{
    struct ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = conn.portNum;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    int mask             = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv.modify_qp(conn.qp.get(), &attr, mask);
}

static void
applyVendorQpAttrs(RcConnection &conn, struct ibv_qp_attr *attr)
{
    if (!conn.ctx || !attr) {
        return;
    }
    struct ibv_device_attr devAttr;
    std::memset(&devAttr, 0, sizeof(devAttr));
    if (ibv.query_device(conn.ctx.get(), &devAttr) != 0) {
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

int
transitionQpToRtr(RcConnection &conn, uint32_t destQpNum, uint16_t destLid, union ibv_gid destGid)
{
    // The address handle stores the GID index in a uint8_t, so an index
    // outside [0, 255] (including the -1 "not selected" default) can't be
    // used and must not be truncated into a different, valid-looking one.
    if (!std::in_range<uint8_t>(conn.gidIndex)) {
        return -1;
    }

    struct ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    applyVendorQpAttrs(conn, &attr);
    attr.dest_qp_num               = destQpNum;
    attr.rq_psn                    = 0;
    attr.max_dest_rd_atomic        = 1;
    attr.min_rnr_timer             = 12;
    attr.ah_attr.is_global         = 1;
    attr.ah_attr.dlid              = destLid;
    attr.ah_attr.sl                = 0;
    attr.ah_attr.src_path_bits     = 0;
    attr.ah_attr.port_num          = conn.portNum;
    attr.ah_attr.grh.dgid          = destGid;
    attr.ah_attr.grh.flow_label    = 0;
    attr.ah_attr.grh.hop_limit     = 64;
    attr.ah_attr.grh.sgid_index    = static_cast<uint8_t>(conn.gidIndex);
    attr.ah_attr.grh.traffic_class = 0;
    int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
               IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    return ibv.modify_qp(conn.qp.get(), &attr, mask);
}

int
transitionQpToRts(RcConnection &conn)
{
    struct ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    applyVendorQpAttrs(conn, &attr);
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    int mask           = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
               IBV_QP_MAX_QP_RD_ATOMIC;
    return ibv.modify_qp(conn.qp.get(), &attr, mask);
}

int
connectRcPeer(RcConnection &conn, const RdmaToken &peerToken)
{
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

int
pollCompletion(RcConnection &conn, int expectedOpcode, int timeoutMs)
{
    if (!conn.cq) {
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        struct ibv_wc wc;
        int           n = ibv.poll_cq(conn.cq.get(), 1, &wc);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            /* The NIC has told us exactly why the transfer died, and this is the
             * only place that number exists. Dropping it here is what turns a
             * specific fault -- a bad rkey, a protection violation, an unreachable
             * remote -- into an indistinguishable -1 several layers up. */
            fprintf(stderr,
                    "hipObj: work completion failed: status=%d opcode=%d "
                    "vendor_err=0x%" PRIx32 " wr_id=%" PRIu64 "\n",
                    static_cast<int>(wc.status), static_cast<int>(wc.opcode), wc.vendor_err, wc.wr_id);
            return -1;
        }
        if (expectedOpcode < 0 || wc.opcode == expectedOpcode) {
            return 0;
        }
    }
    // A timeout is a failure: no completion arrived for the awaited work
    // request, so the transfer outcome is unknown and must not be reported
    // as success.
    fprintf(stderr,
            "hipObj: no work completion within %d ms (awaiting opcode %d); the "
            "transfer outcome is unknown\n",
            timeoutMs, expectedOpcode);
    return -1;
}

} // namespace hipObj
