/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* v2 transport: device/qp ownership split and release procedure.
 *
 * DeviceHandle is the shared half (context, protection domain,
 * topology) with a connection reference count; RcConnV2 owns one
 * qp/cq pair. v2 entry points route through these helpers so the
 * registry, capacity accounting, and the retired ring stay
 * consistent. Everything runs under the library-wide API lock (ApiGuard,
 * in state.h). */

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ibv-core.h"
#include "ibv-ptr.h"
#include "v2-registry.h"

namespace hipObj {

/* pd is declared after ctx so it is deallocated before the context
 * closes. */
struct DeviceHandle {
    IbvContextPtr         ctx;
    IbvPdPtr              pd;
    uint8_t               portNum  = 1;
    int                   gidIndex = -1;
    union ibv_gid         localGid = {};
    std::atomic<uint32_t> connRefs{0};
};

namespace v2 {

    /* Creates the qp/cq pair on dh. On success conn holds both objects,
     * conn.qpNum is captured, and dh.connRefs was incremented. On
     * failure every partially created object was rolled back, except
     * that a cq the rollback couldn't destroy stays in conn.cq, so the
     * caller can raise a poisoned tombstone. */
    int createRcConnV2(DeviceHandle &dh, RcConnV2 &conn);

    /* Whether destroyRcConnV2() destroyed each object. The caller feeds
     * them to commitDestroy(). */
    struct DestroyResult {
        bool qpOk;
        bool cqOk;
    };

    /* Destroys conn's qp and cq. An object that fails to destroy stays in
     * conn for a retry. */
    DestroyResult destroyRcConnV2(RcConnV2 &conn);

    /* QP state transitions taking the device topology from dh. The v1
     * signatures (RcConnection&) remain unchanged. */
    int transitionQpToInitV2(DeviceHandle &dh, RcConnV2 &conn);
    int transitionQpToRtrV2(DeviceHandle &dh, RcConnV2 &conn, uint32_t destQpNum, union ibv_gid destGid,
                            uint32_t rqPsn);
    int transitionQpToRtsV2(RcConnV2 &conn, DeviceHandle &dh, uint32_t sqPsn);

    /* Creates only a qp on an existing cq (conflict-discard retry uses
     * this so the original cq survives). */
    int createRcQpOnly(DeviceHandle &dh, struct ibv_cq *cq, RcConnV2 &conn);

    /* Retires a live qp whose (qpn, psn) was already used by a peer:
     * reserves a fresh ring slot, destroys the old qp, records the
     * tuple, and recreates the qp on the existing cq. Busy means the
     * retired ring is full. Leftover means there was nothing to discard
     * or the old qp wouldn't destroy, which leaves the entry unchanged,
     * or that the new qp couldn't be created, which releases the entry
     * (poisoned, if its cq wouldn't destroy either). */
    ReleaseResult discardAndRecreateQp(ConnId id);

    /* Decrements dh.connRefs after a successful releaseRcConnV2 and
     * closes ctx/pd when no MR and no connection remain. */
    void releaseDevice(DeviceHandle &dh);

    /* Full v2 release procedure for one registry entry: claim, reserve
     * a retired slot for a live qp, destroy, commit, record, erase.
     * Releasing an entry that's gone, or that another caller has
     * claimed, is Ok. Leftover means an object survived its destroy, so
     * the entry stays, poisoned, for a retry. */
    ReleaseResult releaseConnection(ConnId id);

    /* Posts the zero-SGE receive work request (wr_id = kRecvImm). */
    constexpr uint64_t kRecvImm = 0x5245435632494d4dULL; /* "RECV2IMM" */
    int                postRecvImm(RcConnV2 &conn);

    /* Completion validation for the data phase. A GET consumes one
     * RECV_RDMA_WITH_IMM completion; a PUT consumes one RECV completion
     * carrying the immediate (flags & IBV_WC_WITH_IMM). The immediate
     * carries the session cookie in network order - the byte count
     * travels in the FINAL response instead. */
    enum class WcKind { kGet, kPut };

    struct WcExpectation {
        WcKind   kind;
        uint64_t wrId;   /* expected wr_id (kRecvImm for receives) */
        uint32_t cookie; /* expected immediate value (the cookie) */
    };

    /* Checks one work completion against the expectation. Returns why it
     * doesn't match, or std::nullopt when it does. */
    std::optional<std::string_view> dataCompletionMismatch(const struct ibv_wc &wc,
                                                           const WcExpectation &expect);

} // namespace v2
} // namespace hipObj
