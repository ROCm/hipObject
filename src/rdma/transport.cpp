/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "transport.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "hipobj-span.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "rdma-topology.h"
#include "token.h"
#include "vendor-ops.h"

namespace hipObj {

std::optional<RdmaDevice>
openRdmaDevice(const Verbs &verbs, const NicTopology &topology, int nicIndex)
{
    /* Connections use the device's first port */
    constexpr uint8_t kPortNum = 1;

    int  numDevs = 0;
    auto devList = own<IbvDeviceListPtr>(verbs, verbs.get_device_list(&numDevs));
    if (!devList || numDevs <= 0 || nicIndex < 0 || nicIndex >= numDevs) {
        return std::nullopt;
    }
    struct ibv_device *dev = deviceSpan(devList, numDevs)[static_cast<size_t>(nicIndex)];
    if (dev == nullptr) {
        return std::nullopt;
    }
    auto ctx = own<IbvContextPtr>(verbs, verbs.open_device(dev));
    devList.reset();
    if (!ctx) {
        return std::nullopt;
    }
    auto pd = own<IbvPdPtr>(verbs, verbs.alloc_pd(ctx.get()));
    if (!pd) {
        return std::nullopt;
    }
    ibv_port_attr portAttr{};
    if (verbs.query_port(ctx.get(), kPortNum, &portAttr) != 0) {
        return std::nullopt;
    }
    /* The first entry, when none is preferred */
    const int gidIndex = SelectBestGid(verbs, topology, ctx.get(), kPortNum).value_or(0);
    ibv_gid   localGid{};
    if (verbs.query_gid(ctx.get(), kPortNum, gidIndex, &localGid) != 0) {
        return std::nullopt;
    }
    /* Read once here, since it can't change while the device is open, so
     * connecting a QP doesn't have to query it. A device that can't report
     * it still opens, and its QPs get no vendor attributes. */
    std::optional<uint32_t> vendorId;
    ibv_device_attr         devAttr{};
    if (verbs.query_device(ctx.get(), &devAttr) == 0) {
        vendorId = devAttr.vendor_id;
    }
    return RdmaDevice(verbs, std::move(ctx), std::move(pd), kPortNum, gidIndex, localGid, vendorId);
}

std::optional<RdmaDevice>
openRdmaDeviceByName(const Verbs &verbs, const NicTopology &topology, std::string_view devName)
{
    if (devName.empty()) {
        return std::nullopt;
    }
    int        numDevs = 0;
    const auto devList = own<IbvDeviceListPtr>(verbs, verbs.get_device_list(&numDevs));
    if (!devList || numDevs <= 0) {
        return std::nullopt;
    }
    const std::span<struct ibv_device *> devices  = deviceSpan(devList, numDevs);
    int                                  nicIndex = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        const char *name = verbs.get_device_name(devices[i]);
        if (name != nullptr && std::string_view(name) == devName) {
            nicIndex = static_cast<int>(i);
            break;
        }
    }
    if (nicIndex < 0) {
        return std::nullopt;
    }
    return openRdmaDevice(verbs, topology, nicIndex);
}

namespace {

    [[nodiscard]] bool transitionQpToInit(RcConnection &conn)
    {
        struct ibv_qp_attr attr;
        zeroObject(attr);
        attr.qp_state        = IBV_QPS_INIT;
        attr.pkey_index      = 0;
        attr.port_num        = conn.device().portNum();
        attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
        const int mask       = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
        return conn.device().verbs().modify_qp(conn.qp(), &attr, mask) == 0;
    }

    void applyVendorQpAttrs(const RdmaDevice &device, struct ibv_qp_attr &attr)
    {
        const std::optional<uint32_t> vendorId = device.vendorId();
        if (!vendorId) {
            return;
        }
#ifdef HIPOBJ_BNXT
        if (isBnxtDevice(*vendorId)) {
            configureBnxtQp(attr);
        }
#endif
#ifdef HIPOBJ_IONIC
        if (isIonicDevice(*vendorId)) {
            configureIonicQp(attr);
        }
#endif
    }

} // namespace

std::optional<RcConnection>
createRcConnection(const RdmaDevice &device, int cqSize, uint32_t maxSendWr, uint32_t maxRecvWr)
{
    const Verbs &verbs = device.verbs();
    auto         cq    = own<IbvCqPtr>(verbs, verbs.create_cq(device.context(), cqSize, nullptr, nullptr, 0));
    if (!cq) {
        return std::nullopt;
    }
    struct ibv_qp_init_attr initAttr;
    zeroObject(initAttr);
    initAttr.send_cq          = cq.get();
    initAttr.recv_cq          = cq.get();
    initAttr.cap.max_send_wr  = maxSendWr;
    initAttr.cap.max_recv_wr  = maxRecvWr;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;
    initAttr.qp_type          = IBV_QPT_RC;
    auto qp                   = own<IbvQpPtr>(verbs, verbs.create_qp(device.pd(), &initAttr));
    if (!qp) {
        return std::nullopt;
    }
    /* Destroyed, releasing the QP and then the CQ, if the transition
     * fails */
    RcConnection conn(device, std::move(cq), std::move(qp));
    if (!transitionQpToInit(conn)) {
        return std::nullopt;
    }
    return conn;
}

bool
transitionQpToRtr(RcConnection &conn, uint32_t destQpNum, uint16_t destLid, union ibv_gid destGid)
{
    // The address handle stores the GID index in a uint8_t, so an index
    // outside [0, 255] can't be used and must not be truncated into a
    // different, valid-looking one.
    const RdmaDevice &device = conn.device();
    if (!std::in_range<uint8_t>(device.gidIndex())) {
        return false;
    }

    struct ibv_qp_attr attr;
    zeroObject(attr);
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    applyVendorQpAttrs(device, attr);
    attr.dest_qp_num               = destQpNum;
    attr.rq_psn                    = 0;
    attr.max_dest_rd_atomic        = 1;
    attr.min_rnr_timer             = 12;
    attr.ah_attr.is_global         = 1;
    attr.ah_attr.dlid              = destLid;
    attr.ah_attr.sl                = 0;
    attr.ah_attr.src_path_bits     = 0;
    attr.ah_attr.port_num          = device.portNum();
    attr.ah_attr.grh.dgid          = destGid;
    attr.ah_attr.grh.flow_label    = 0;
    attr.ah_attr.grh.hop_limit     = 64;
    attr.ah_attr.grh.sgid_index    = static_cast<uint8_t>(device.gidIndex());
    attr.ah_attr.grh.traffic_class = 0;
    const int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                     IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    return device.verbs().modify_qp(conn.qp(), &attr, mask) == 0;
}

bool
transitionQpToRts(RcConnection &conn)
{
    struct ibv_qp_attr attr;
    zeroObject(attr);
    attr.qp_state = IBV_QPS_RTS;
    applyVendorQpAttrs(conn.device(), attr);
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    const int mask     = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                     IBV_QP_MAX_QP_RD_ATOMIC;
    return conn.device().verbs().modify_qp(conn.qp(), &attr, mask) == 0;
}

bool
connectRcPeer(RcConnection &conn, const RdmaToken &peerToken)
{
    if (peerToken.transport != TransportType::RC) {
        return false;
    }
    if (!transitionQpToRtr(conn, peerToken.qpNum, peerToken.lid, toIbvGid(peerToken.gid))) {
        return false;
    }
    return transitionQpToRts(conn);
}

namespace {

    [[nodiscard]] bool transitionQpToReset(RcConnection &conn)
    {
        struct ibv_qp_attr attr;
        zeroObject(attr);
        attr.qp_state = IBV_QPS_RESET;
        return conn.device().verbs().modify_qp(conn.qp(), &attr, IBV_QP_STATE) == 0;
    }

    /* Polls conn's CQ until it's empty, and false if polling fails. Once
     * the QP is in RESET, nothing more completes on it, and the CQ holds
     * at most cqe completions, so that's as many polls as it can take. */
    [[nodiscard]] bool drainCompletions(RcConnection &conn)
    {
        const Verbs &verbs = conn.device().verbs();
        for (int left = conn.cq()->cqe; left > 0; --left) {
            ibv_wc    wc{};
            const int n = verbs.poll_cq(conn.cq(), 1, &wc);
            if (n < 0) {
                return false;
            }
            if (n == 0) {
                break;
            }
        }
        return true;
    }

} // namespace

bool
resetRcConnection(RcConnection &conn)
{
    return transitionQpToReset(conn) && drainCompletions(conn) && transitionQpToInit(conn);
}

bool
pollCompletion(RcConnection &conn, std::optional<ibv_wc_opcode> expectedOpcode,
               std::chrono::milliseconds timeout)
{
    const Verbs &verbs    = conn.device().verbs();
    const auto   deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ibv_wc    wc{};
        const int n = verbs.poll_cq(conn.cq(), 1, &wc);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            /* The NIC has told us exactly why the transfer died, and this is the
             * only place that number exists. Dropping it here is what turns a
             * specific fault -- a bad rkey, a protection violation, an unreachable
             * remote -- into an indistinguishable failure several layers up. */
            fprintf(stderr,
                    "hipObj: work completion failed: status=%d opcode=%d "
                    "vendor_err=0x%" PRIx32 " wr_id=%" PRIu64 "\n",
                    static_cast<int>(wc.status), static_cast<int>(wc.opcode), wc.vendor_err, wc.wr_id);
            return false;
        }
        if (!expectedOpcode || wc.opcode == *expectedOpcode) {
            return true;
        }
    }
    // A timeout is a failure: no completion arrived for the awaited work
    // request, so the transfer outcome is unknown and must not be reported
    // as success.
    const int64_t     timeoutMs = timeout.count();
    const std::string awaited =
        expectedOpcode ? "opcode " + std::to_string(static_cast<int>(*expectedOpcode)) : "any opcode";
    fprintf(stderr,
            "hipObj: no work completion within %" PRId64 " ms (awaiting %s); the "
            "transfer outcome is unknown\n",
            timeoutMs, awaited.c_str());
    return false;
}

} // namespace hipObj
