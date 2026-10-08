/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <utility>

#include "ibv-core.h"
#include "ibv-ptr.h"
#include "replace-by-move.h"

namespace hipObj {

struct RdmaToken;

/* Members are destroyed in reverse order (qp, cq, pd, ctx), which is the
 * order ibverbs requires. Move assignment destroys the target first, so
 * it releases them in that order too. */
struct RcConnection final {
    IbvContextPtr ctx;
    IbvPdPtr      pd;
    IbvCqPtr      cq;
    IbvQpPtr      qp;
    uint8_t       portNum  = 1;
    int           gidIndex = -1;
    union ibv_gid localGid = {};

    RcConnection()                                = default;
    ~RcConnection()                               = default;
    RcConnection(const RcConnection &)            = delete;
    RcConnection &operator=(const RcConnection &) = delete;
    RcConnection(RcConnection &&) noexcept        = default;
    RcConnection &operator=(RcConnection &&other) noexcept
    {
        replaceByMove(*this, std::move(other));
        return *this;
    }
};

int  openRdmaDevice(int nicIndex, RcConnection &conn);
int  openRdmaDeviceByName(const char *devName, RcConnection &conn);
void closeRdmaDevice(RcConnection &conn);
int  createRcQp(RcConnection &conn, int cqSize, uint32_t maxSendWr, uint32_t maxRecvWr);
int  transitionQpToInit(RcConnection &conn);
int  transitionQpToRtr(RcConnection &conn, uint32_t destQpNum, uint16_t destLid, union ibv_gid destGid);
int  transitionQpToRts(RcConnection &conn);
int  connectRcPeer(RcConnection &conn, const RdmaToken &peerToken);
int  pollCompletion(RcConnection &conn, int expectedOpcode, int timeoutMs);

} // namespace hipObj
