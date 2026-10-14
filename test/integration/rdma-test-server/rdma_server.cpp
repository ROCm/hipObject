/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "rdma_server.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "hipobj-span.h"
#include "ibv-wrapper.h"
#include "malloc_ptr.h"
#include "nic-seam.h"
#include "scope-guard.h"
#include "token.h"
#include "token_header.h"
#include "transport.h"

namespace hipobj::test {

namespace {

    /* Every failure below used to be a bare `return -1`, which the HTTP layer
     * turns into "500 RDMA PUT failed" with no reply header -- indistinguishable,
     * from the client, from a server that does not speak the RDMA extension at
     * all. Six different faults reported as one. Name which one it was. */
    int rdmaFail(const std::string &op, const std::string &why)
    {
        fprintf(stderr, "hipobj-rdma-test-server: %s failed: %s\n", op.c_str(), why.c_str());
        return -1;
    }

    /* The size of the buffer each transfer is staged through */
    constexpr size_t kStagingSize = 64 * 1024 * 1024;

    hipObj::RdmaToken buildServerToken(const hipObj::RcConnection &conn, struct ibv_mr *mr, size_t size)
    {
        hipObj::RdmaToken token{};
        token.transport  = hipObj::TransportType::RC;
        token.qpNum      = conn.qp()->qp_num;
        token.gid        = hipObj::toGid(conn.device().localGid());
        token.rkey       = mr->rkey;
        token.remoteAddr = reinterpret_cast<uint64_t>(mr->addr);
        token.length     = size;
        token.portNum    = conn.device().portNum();
        token.lid        = 0;
        return token;
    }

    int postRdmaWrite(hipObj::RcConnection &conn, struct ibv_mr *localMr, size_t size,
                      const hipObj::RdmaToken &clientToken)
    {
        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uint64_t>(localMr->addr);
        sge.length = static_cast<uint32_t>(size);
        sge.lkey   = localMr->lkey;

        ibv_send_wr wr{};
        wr.wr_id               = 1;
        wr.opcode              = IBV_WR_RDMA_WRITE;
        wr.send_flags          = IBV_SEND_SIGNALED;
        wr.sg_list             = &sge;
        wr.num_sge             = 1;
        wr.wr.rdma.remote_addr = clientToken.remoteAddr;
        wr.wr.rdma.rkey        = clientToken.rkey;

        ibv_send_wr *bad = nullptr;
        if (conn.device().verbs().post_send(conn.qp(), &wr, &bad) != 0) {
            return -1;
        }
        return hipObj::pollCompletion(conn, IBV_WC_RDMA_WRITE, std::chrono::seconds(10));
    }

    int postRdmaRead(hipObj::RcConnection &conn, struct ibv_mr *localMr, size_t size,
                     const hipObj::RdmaToken &clientToken)
    {
        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uint64_t>(localMr->addr);
        sge.length = static_cast<uint32_t>(size);
        sge.lkey   = localMr->lkey;

        ibv_send_wr wr{};
        wr.wr_id               = 2;
        wr.opcode              = IBV_WR_RDMA_READ;
        wr.send_flags          = IBV_SEND_SIGNALED;
        wr.sg_list             = &sge;
        wr.num_sge             = 1;
        wr.wr.rdma.remote_addr = clientToken.remoteAddr;
        wr.wr.rdma.rkey        = clientToken.rkey;

        ibv_send_wr *bad = nullptr;
        if (conn.device().verbs().post_send(conn.qp(), &wr, &bad) != 0) {
            return -1;
        }
        return hipObj::pollCompletion(conn, IBV_WC_RDMA_READ, std::chrono::seconds(10));
    }

} // namespace

struct RdmaTestServer::Impl {
    /* Creates the connection, on the device it now holds */
    Impl(hipObj::RdmaDevice rdmaDevice, hipObj::MallocPtr buf, hipObj::IbvMrPtr mr)
        : device(std::move(rdmaDevice)), stagingBuf(std::move(buf)), stagingMr(std::move(mr)),
          conn(createConnection(device))
    {
    }

    /* Destroyed in reverse order: the QP and CQ are destroyed, and the MR
     * deregistered and its buffer freed, before the device's PD and
     * context go away. */
    hipObj::RdmaDevice device;
    hipObj::MallocPtr  stagingBuf;
    hipObj::IbvMrPtr   stagingMr;
    /* Empty when the server can't serve RDMA */
    std::optional<hipObj::RcConnection> conn;

    static std::optional<hipObj::RcConnection> createConnection(const hipObj::RdmaDevice &rdmaDevice)
    {
        return hipObj::createRcConnection(rdmaDevice, 256, 128, 128);
    }

    /* The staging buffer's bytes */
    std::span<uint8_t> staging() const
    {
        return hipObj::spanFromPointer(static_cast<uint8_t *>(stagingBuf.get()), kStagingSize);
    }

    /* One QP serves every request in turn, so it has to come back to INIT
     * between them -- an RC QP already in RTS cannot be moved to RTR for the
     * next client. Only the success paths used to do this, which meant a
     * single failed transfer wedged the server for every client after it:
     * each one got "could not connect the RC QP to the client", naming the
     * wrong fault and hiding the real one. Run it on every path that touched
     * the QP, failure included. */
    void resetQp()
    {
        if (!conn) {
            return;
        }
        /* The old QP and CQ are destroyed before the new ones are created */
        conn.reset();
        std::optional<hipObj::RcConnection> fresh = createConnection(device);
        if (!fresh) {
            fprintf(stderr, "hipobj-rdma-test-server: could not rebuild the RC QP; "
                            "the server can no longer serve RDMA\n");
            return;
        }
        conn.emplace(std::move(*fresh));
    }
};

RdmaTestServer::RdmaTestServer(const hipObj::Verbs *verbsPtr)
{
    if (verbsPtr == nullptr) {
        return;
    }
    /* Built in locals and moved into impl_ only once everything has
     * succeeded, so a failure, or an exception, destroys them in reverse
     * order with no rollback code */
    const hipObj::Verbs              &verbs = *verbsPtr;
    const hipObj::SysfsTopology       topology(verbs);
    std::optional<hipObj::RdmaDevice> device = hipObj::openRdmaDevice(verbs, topology, 0);
    if (!device) {
        return;
    }
    hipObj::MallocPtr stagingBuf(std::malloc(kStagingSize));
    if (!stagingBuf) {
        return;
    }
    hipObj::IbvMrPtr stagingMr = hipObj::own<hipObj::IbvMrPtr>(
        verbs, verbs.reg_mr(device->pd(), stagingBuf.get(), kStagingSize,
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
    if (!stagingMr) {
        return;
    }
    auto impl = std::make_unique<Impl>(std::move(*device), std::move(stagingBuf), std::move(stagingMr));
    if (!impl->conn) {
        return;
    }
    impl_ = std::move(impl);
}

RdmaTestServer::~RdmaTestServer() = default;

bool
RdmaTestServer::isReady() const
{
    return impl_ && impl_->conn;
}

int
RdmaTestServer::rdmaWriteToClient(const TokenHeader &tokenHeader, const std::vector<uint8_t> &data,
                                  std::string &replyHeader)
{
    if (!isReady()) {
        return rdmaFail("GET", "server RDMA not initialised");
    }
    const hipObj::RdmaToken &clientToken = tokenHeader.token;
    size_t                   xferSize    = tokenHeader.length;
    if (xferSize == 0) {
        xferSize = data.size();
    }
    if (data.size() < xferSize || xferSize > kStagingSize) {
        fprintf(stderr,
                "hipobj-rdma-test-server: GET failed: %zu bytes requested of a %zu "
                "byte object, %zu byte staging buffer\n",
                xferSize, data.size(), kStagingSize);
        return -1;
    }

    std::ranges::copy(std::span(data).first(xferSize), impl_->staging().begin());
    /* Connecting touches the QP, so every way out from here resets it */
    const hipObj::ScopeGuard resetQp([this] { impl_->resetQp(); });
    if (hipObj::connectRcPeer(*impl_->conn, clientToken) != 0) {
        return rdmaFail("GET", "could not connect the RC QP to the client");
    }
    if (postRdmaWrite(*impl_->conn, impl_->stagingMr.get(), xferSize, clientToken) != 0) {
        return rdmaFail("GET", "RDMA WRITE to the client buffer did not complete");
    }
    hipObj::RdmaToken serverToken = buildServerToken(*impl_->conn, impl_->stagingMr.get(), xferSize);
    replyHeader                   = hipObj::encodeReplyWithPeerToken(200, serverToken);
    return 0;
}

int
RdmaTestServer::rdmaReadFromClient(const TokenHeader &tokenHeader, size_t size, std::vector<uint8_t> &data,
                                   std::string &replyHeader)
{
    if (!isReady()) {
        return rdmaFail("PUT", "server RDMA not initialised");
    }
    const hipObj::RdmaToken &clientToken = tokenHeader.token;
    size_t                   xferSize    = tokenHeader.length;
    if (size != 0) {
        xferSize = size;
    }
    if (xferSize == 0) {
        xferSize = clientToken.length;
    }
    if (xferSize > kStagingSize) {
        fprintf(stderr,
                "hipobj-rdma-test-server: PUT failed: %zu bytes exceeds the %zu "
                "byte staging buffer\n",
                xferSize, kStagingSize);
        return -1;
    }

    /* Connecting touches the QP, so every way out from here resets it */
    const hipObj::ScopeGuard resetQp([this] { impl_->resetQp(); });
    if (hipObj::connectRcPeer(*impl_->conn, clientToken) != 0) {
        return rdmaFail("PUT", "could not connect the RC QP to the client");
    }
    if (postRdmaRead(*impl_->conn, impl_->stagingMr.get(), xferSize, clientToken) != 0) {
        return rdmaFail("PUT", "RDMA READ from the client buffer did not complete");
    }
    const std::span<const uint8_t> received = impl_->staging().first(xferSize);
    data.assign(received.begin(), received.end());
    hipObj::RdmaToken serverToken = buildServerToken(*impl_->conn, impl_->stagingMr.get(), xferSize);
    replyHeader                   = hipObj::encodeReplyWithPeerToken(200, serverToken);
    return 0;
}

} // namespace hipobj::test
