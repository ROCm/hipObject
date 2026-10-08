/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

#include "ibv-core.h"
#include "ibv-ptr.h"

namespace hipObj {

struct RdmaToken;

/* Members are destroyed in reverse order (qp, cq, pd, ctx), which is the
 * order ibverbs requires. Move-assignment replaces them in declaration
 * order instead, so only move into a connection that is empty. */
struct RcConnection {
    IbvContextPtr ctx;
    IbvPdPtr      pd;
    IbvCqPtr      cq;
    IbvQpPtr      qp;
    uint8_t       portNum  = 1;
    int           gidIndex = -1;
    union ibv_gid localGid = {};
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
