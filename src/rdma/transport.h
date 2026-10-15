/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "ibv-core.h"
#include "ibv-ptr.h"

namespace hipObj {

class NicTopology;
class Verbs;
struct RdmaToken;

/* An open RDMA device: its context and protection domain, the verbs they
 * came from, the port and GID that connections on it use, and its vendor.
 * Nothing changes once it's built, so several threads can use it at once.
 *
 * pd_ is declared after ctx_, so the protection domain is deallocated
 * before the context closes. verbs has to outlive it. */
class RdmaDevice final {
public:
    RdmaDevice(const Verbs &verbs, IbvContextPtr ctx, IbvPdPtr pd, uint8_t portNum, int gidIndex,
               const ibv_gid &localGid, std::optional<uint32_t> vendorId) noexcept
        : verbs_(&verbs), ctx_(std::move(ctx)), pd_(std::move(pd)), portNum_(portNum), gidIndex_(gidIndex),
          localGid_(localGid), vendorId_(vendorId)
    {
    }
    ~RdmaDevice() = default;

    /* Moved into place once, and never copied or assigned */
    RdmaDevice(const RdmaDevice &)            = delete;
    RdmaDevice &operator=(const RdmaDevice &) = delete;
    RdmaDevice(RdmaDevice &&) noexcept        = default;
    RdmaDevice &operator=(RdmaDevice &&)      = delete;

    [[nodiscard]] const Verbs &verbs() const noexcept
    {
        return *verbs_;
    }

    [[nodiscard]] struct ibv_context *context() const noexcept
    {
        return ctx_.get();
    }

    [[nodiscard]] struct ibv_pd *pd() const noexcept
    {
        return pd_.get();
    }

    [[nodiscard]] uint8_t portNum() const noexcept
    {
        return portNum_;
    }

    /* The index of localGid() in the port's GID table */
    [[nodiscard]] int gidIndex() const noexcept
    {
        return gidIndex_;
    }

    [[nodiscard]] const ibv_gid &localGid() const noexcept
    {
        return localGid_;
    }

    /* The vendor ID the device reported when it opened, or std::nullopt if
     * it couldn't be queried */
    [[nodiscard]] std::optional<uint32_t> vendorId() const noexcept
    {
        return vendorId_;
    }

private:
    const Verbs            *verbs_;
    IbvContextPtr           ctx_;
    IbvPdPtr                pd_;
    uint8_t                 portNum_;
    int                     gidIndex_;
    ibv_gid                 localGid_;
    std::optional<uint32_t> vendorId_;
};

/* A reliable-connected QP, and the CQ its work completes on, on a device
 * that has to outlive it. createRcConnection() creates one, with its QP in
 * INIT. qp_ is declared after cq_, so the QP is destroyed before its CQ. */
class RcConnection final {
public:
    /* Owns cq and qp, which were created on device, and are both set */
    RcConnection(const RdmaDevice &device, IbvCqPtr cq, IbvQpPtr qp) noexcept
        : device_(&device), cq_(std::move(cq)), qp_(std::move(qp))
    {
    }
    /* A temporary device would be destroyed before the connection */
    RcConnection(const RdmaDevice &&, IbvCqPtr, IbvQpPtr) = delete;
    ~RcConnection()                                       = default;

    /* Moving leaves the device where it is, so the connection still refers
     * to it. A defaulted move assignment would replace cq_ before qp_,
     * destroying the old CQ while the old QP still used it, so there's
     * none; replace a connection in a std::optional with reset() and
     * emplace(). */
    RcConnection(const RcConnection &)            = delete;
    RcConnection &operator=(const RcConnection &) = delete;
    RcConnection(RcConnection &&) noexcept        = default;
    RcConnection &operator=(RcConnection &&)      = delete;

    [[nodiscard]] const RdmaDevice &device() const noexcept
    {
        return *device_;
    }

    [[nodiscard]] struct ibv_cq *cq() const noexcept
    {
        return cq_.get();
    }

    [[nodiscard]] struct ibv_qp *qp() const noexcept
    {
        return qp_.get();
    }

private:
    const RdmaDevice *device_;
    IbvCqPtr          cq_;
    IbvQpPtr          qp_;
};

/* Opens device nicIndex in verbs' device list, choosing its GID with the
 * RoCE versions topology reports. std::nullopt, with nothing left open,
 * when a step fails. A device that can't report its vendor still opens,
 * with no vendor ID. */
std::optional<RdmaDevice> openRdmaDevice(const Verbs &verbs, const NicTopology &topology, int nicIndex);
/* openRdmaDevice() on the device named devName. std::nullopt when no
 * device has that name, which an empty name never is. */
std::optional<RdmaDevice> openRdmaDeviceByName(const Verbs &verbs, const NicTopology &topology,
                                               std::string_view devName);

/* A connection on device, which has to outlive it, with a CQ of cqSize
 * entries and a QP in INIT that holds maxSendWr and maxRecvWr work
 * requests. std::nullopt, with nothing left created, when a step fails. */
std::optional<RcConnection> createRcConnection(const RdmaDevice &device, int cqSize, uint32_t maxSendWr,
                                               uint32_t maxRecvWr);
/* A temporary device would be destroyed before the connection */
std::optional<RcConnection> createRcConnection(const RdmaDevice &&, int, uint32_t, uint32_t) = delete;

/* Each of these returns true on success and false on failure */
[[nodiscard]] bool transitionQpToRtr(RcConnection &conn, uint32_t destQpNum, uint16_t destLid,
                                     union ibv_gid destGid);
[[nodiscard]] bool transitionQpToRts(RcConnection &conn);
[[nodiscard]] bool connectRcPeer(RcConnection &conn, const RdmaToken &peerToken);
/* Fails on an error completion, or when no completion with expectedOpcode
 * (any opcode, if it's std::nullopt) arrives within timeout */
[[nodiscard]] bool pollCompletion(RcConnection &conn, std::optional<ibv_wc_opcode> expectedOpcode,
                                  std::chrono::milliseconds timeout);

} // namespace hipObj
