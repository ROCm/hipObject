/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "rdma_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ibv-wrapper.h"
#include "token.h"
#include "transport.h"

namespace hipobj::test {

namespace {

    constexpr int IBV_ACCESS_LOCAL_WRITE  = 0x4;
    constexpr int IBV_ACCESS_REMOTE_READ  = 0x1;
    constexpr int IBV_ACCESS_REMOTE_WRITE = 0x2;

    /* Every failure below used to be a bare `return -1`, which the HTTP layer
     * turns into "500 RDMA PUT failed" with no reply header -- indistinguishable,
     * from the client, from a server that does not speak the RDMA extension at
     * all. Six different faults reported as one. Name which one it was. */
    int rdmaFail(const char *op, const char *why)
    {
        fprintf(stderr, "hipobj-rdma-test-server: %s failed: %s\n", op, why);
        return -1;
    }

    bool parseTokenHeader(const std::string &header, hipObj::RdmaToken &token, uint64_t &remoteAddr,
                          size_t &size)
    {
        size_t c1 = header.find(':');
        if (c1 == std::string::npos) {
            remoteAddr = 0;
            size       = 0;
            return hipObj::decodeRdmaTokenHex(header.c_str(), token);
        }
        size_t c2 = header.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            return false;
        }
        std::string tokenHex = header.substr(0, c1);
        if (!hipObj::decodeRdmaTokenHex(tokenHex.c_str(), token)) {
            return false;
        }
        remoteAddr = std::strtoull(header.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16);
        size       = static_cast<size_t>(std::strtoull(header.substr(c2 + 1).c_str(), nullptr, 16));
        if (remoteAddr != 0) {
            token.remoteAddr = remoteAddr;
        }
        if (size != 0) {
            token.length = size;
        }
        return true;
    }

    hipObj::RdmaToken buildServerToken(const hipObj::RcConnection &conn, struct ibv_mr *mr, size_t size)
    {
        hipObj::RdmaToken token{};
        token.transport = hipObj::TRANSPORT_RC;
        token.qpNum     = conn.qp->qp_num;
        std::memcpy(token.gid, conn.localGid.raw, 16);
        token.rkey       = mr->rkey;
        token.remoteAddr = reinterpret_cast<uint64_t>(mr->addr);
        token.length     = size;
        token.portNum    = conn.portNum;
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
        if (hipObj::ibv.post_send(conn.qp, &wr, &bad) != 0) {
            return -1;
        }
        return hipObj::pollCompletion(conn, IBV_WC_RDMA_WRITE, 10000);
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
        if (hipObj::ibv.post_send(conn.qp, &wr, &bad) != 0) {
            return -1;
        }
        return hipObj::pollCompletion(conn, IBV_WC_RDMA_READ, 10000);
    }

} // namespace

struct RdmaTestServer::Impl {
    hipObj::RcConnection conn{};
    struct ibv_mr       *stagingMr   = nullptr;
    void                *stagingBuf  = nullptr;
    size_t               stagingSize = 64 * 1024 * 1024;
    bool                 ready       = false;

    /* One QP serves every request in turn, so it has to come back to INIT
     * between them -- an RC QP already in RTS cannot be moved to RTR for the
     * next client. Only the success paths used to do this, which meant a
     * single failed transfer wedged the server for every client after it:
     * each one got "could not connect the RC QP to the client", naming the
     * wrong fault and hiding the real one. Run it on every path that touched
     * the QP, failure included. */
    void resetQp()
    {
        if (!conn.qp) {
            return;
        }
        hipObj::ibv.destroy_qp(conn.qp);
        conn.qp = nullptr;
        if (hipObj::createRcQp(conn, 256, 128, 128) != 0 || hipObj::transitionQpToInit(conn) != 0) {
            fprintf(stderr, "hipobj-rdma-test-server: could not rebuild the RC QP; "
                            "the server can no longer serve RDMA\n");
            ready = false;
        }
    }

    ~Impl()
    {
        if (stagingMr) {
            hipObj::ibv.dereg_mr(stagingMr);
        }
        std::free(stagingBuf);
        hipObj::closeRdmaDevice(conn);
    }
};

RdmaTestServer::RdmaTestServer() : impl_(std::make_unique<Impl>())
{
    if (!hipObj::ibv.is_initialized) {
        return;
    }
    if (hipObj::openRdmaDevice(0, impl_->conn) != 0) {
        return;
    }
    impl_->stagingBuf = std::malloc(impl_->stagingSize);
    if (!impl_->stagingBuf) {
        hipObj::closeRdmaDevice(impl_->conn);
        return;
    }
    impl_->stagingMr =
        hipObj::ibv.reg_mr_host(impl_->conn.pd, impl_->stagingBuf, impl_->stagingSize,
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!impl_->stagingMr) {
        std::free(impl_->stagingBuf);
        impl_->stagingBuf = nullptr;
        hipObj::closeRdmaDevice(impl_->conn);
        return;
    }
    if (hipObj::createRcQp(impl_->conn, 256, 128, 128) != 0 || hipObj::transitionQpToInit(impl_->conn) != 0) {
        hipObj::ibv.dereg_mr(impl_->stagingMr);
        impl_->stagingMr = nullptr;
        std::free(impl_->stagingBuf);
        impl_->stagingBuf = nullptr;
        hipObj::closeRdmaDevice(impl_->conn);
        return;
    }
    impl_->ready = true;
}

RdmaTestServer::~RdmaTestServer() = default;

bool
RdmaTestServer::isReady() const
{
    return impl_ && impl_->ready;
}

int
RdmaTestServer::rdmaWriteToClient(const std::string &tokenHeader, const std::vector<uint8_t> &data,
                                  std::string &replyHeader)
{
    if (!isReady()) {
        return rdmaFail("GET", "server RDMA not initialised");
    }
    hipObj::RdmaToken clientToken{};
    uint64_t          remoteAddr = 0;
    size_t            xferSize   = 0;
    if (!parseTokenHeader(tokenHeader, clientToken, remoteAddr, xferSize)) {
        return rdmaFail("GET", "x-amz-rdma-token did not parse");
    }
    if (xferSize == 0) {
        xferSize = data.size();
    }
    if (data.size() < xferSize || xferSize > impl_->stagingSize) {
        fprintf(stderr,
                "hipobj-rdma-test-server: GET failed: %zu bytes requested of a %zu "
                "byte object, %zu byte staging buffer\n",
                xferSize, data.size(), impl_->stagingSize);
        return -1;
    }

    std::memcpy(impl_->stagingBuf, data.data(), xferSize);
    if (hipObj::connectRcPeer(impl_->conn, clientToken) != 0) {
        impl_->resetQp();
        return rdmaFail("GET", "could not connect the RC QP to the client");
    }
    if (postRdmaWrite(impl_->conn, impl_->stagingMr, xferSize, clientToken) != 0) {
        impl_->resetQp();
        return rdmaFail("GET", "RDMA WRITE to the client buffer did not complete");
    }
    hipObj::RdmaToken serverToken = buildServerToken(impl_->conn, impl_->stagingMr, xferSize);
    replyHeader                   = hipObj::encodeReplyWithPeerToken(200, serverToken);
    impl_->resetQp();
    return 0;
}

int
RdmaTestServer::rdmaReadFromClient(const std::string &tokenHeader, size_t size, std::vector<uint8_t> &data,
                                   std::string &replyHeader)
{
    if (!isReady()) {
        return rdmaFail("PUT", "server RDMA not initialised");
    }
    hipObj::RdmaToken clientToken{};
    uint64_t          remoteAddr = 0;
    size_t            xferSize   = 0;
    if (!parseTokenHeader(tokenHeader, clientToken, remoteAddr, xferSize)) {
        return rdmaFail("PUT", "x-amz-rdma-token did not parse");
    }
    if (size != 0) {
        xferSize = size;
    }
    if (xferSize == 0) {
        xferSize = clientToken.length;
    }
    if (xferSize > impl_->stagingSize) {
        fprintf(stderr,
                "hipobj-rdma-test-server: PUT failed: %zu bytes exceeds the %zu "
                "byte staging buffer\n",
                xferSize, impl_->stagingSize);
        return -1;
    }

    if (hipObj::connectRcPeer(impl_->conn, clientToken) != 0) {
        impl_->resetQp();
        return rdmaFail("PUT", "could not connect the RC QP to the client");
    }
    if (postRdmaRead(impl_->conn, impl_->stagingMr, xferSize, clientToken) != 0) {
        impl_->resetQp();
        return rdmaFail("PUT", "RDMA READ from the client buffer did not complete");
    }
    data.assign(static_cast<uint8_t *>(impl_->stagingBuf),
                static_cast<uint8_t *>(impl_->stagingBuf) + xferSize);
    hipObj::RdmaToken serverToken = buildServerToken(impl_->conn, impl_->stagingMr, xferSize);
    replyHeader                   = hipObj::encodeReplyWithPeerToken(200, serverToken);
    impl_->resetQp();
    return 0;
}

} // namespace hipobj::test
