/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
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

    const Verbs &verbs() const noexcept
    {
        return *verbs_;
    }

    struct ibv_context *context() const noexcept
    {
        return ctx_.get();
    }

    struct ibv_pd *pd() const noexcept
    {
        return pd_.get();
    }

    uint8_t portNum() const noexcept
    {
        return portNum_;
    }

    /* The index of localGid() in the port's GID table */
    int gidIndex() const noexcept
    {
        return gidIndex_;
    }

    const ibv_gid &localGid() const noexcept
    {
        return localGid_;
    }

    /* The vendor ID the device reported when it opened, or std::nullopt if
     * it couldn't be queried */
    std::optional<uint32_t> vendorId() const noexcept
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
 * that has to outlive it. createRcQp() creates them. qp is declared after
 * cq, so the QP is destroyed before its CQ. */
class RcConnection final {
public:
    explicit RcConnection(const RdmaDevice &device) noexcept : device_(&device)
    {
    }
    /* A temporary device would be destroyed before the connection */
    explicit RcConnection(const RdmaDevice &&) = delete;
    ~RcConnection()                            = default;

    /* Lives in one place, and isn't copied or moved */
    RcConnection(const RcConnection &)            = delete;
    RcConnection &operator=(const RcConnection &) = delete;
    RcConnection(RcConnection &&)                 = delete;
    RcConnection &operator=(RcConnection &&)      = delete;

    const RdmaDevice &device() const noexcept
    {
        return *device_;
    }

    IbvCqPtr cq{};
    IbvQpPtr qp{};

private:
    const RdmaDevice *device_;
};

/* Opens device nicIndex in verbs' device list, choosing its GID with the
 * RoCE versions topology reports. std::nullopt, with nothing left open,
 * when a step fails. A device that can't report its vendor still opens,
 * with no vendor ID. */
std::optional<RdmaDevice> openRdmaDevice(const Verbs &verbs, const NicTopology &topology, int nicIndex);
std::optional<RdmaDevice> openRdmaDeviceByName(const Verbs &verbs, const NicTopology &topology,
                                               const char *devName);

int createRcQp(RcConnection &conn, int cqSize, uint32_t maxSendWr, uint32_t maxRecvWr);
int transitionQpToInit(RcConnection &conn);
int transitionQpToRtr(RcConnection &conn, uint32_t destQpNum, uint16_t destLid, union ibv_gid destGid);
int transitionQpToRts(RcConnection &conn);
int connectRcPeer(RcConnection &conn, const RdmaToken &peerToken);
int pollCompletion(RcConnection &conn, int expectedOpcode, std::chrono::milliseconds timeout);

} // namespace hipObj
