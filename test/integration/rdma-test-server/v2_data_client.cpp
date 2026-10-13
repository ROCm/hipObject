/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Data-plane client for the v2 reference-server E2E: pairs a QP
 * with the server session described by the PREPARE/READY exchange
 * and performs the actual RDMA transfer (WRITE_WITH_IMM for PUT,
 * READ pull for GET) so the server data phase runs for real. */

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../../src/rdma/token.h"
#include "gid.h"
#include "hipobj-format.h"
#include "hipobj-parse.h"
#include "hipobj-span.h"
#include "http_status.h"
#include "malloc_ptr.h"
#include "unique-fd.h"
#include "v2_sigv4.h"

namespace {

/* QPNs and PSNs are 24-bit fields. QP 0 is reserved, and the protocol
 * doesn't use PSN 0. */
constexpr uint32_t kMax24Bit = 0xffffff;

/* The protocol sends the server's PSN as exactly this many hex digits */
constexpr size_t kPsnHexLen = 6;

/* Size of the transfer buffer: one page */
constexpr size_t kBufBytes = 4096;

/* Owning pointers for verbs objects. This client calls libibverbs
 * directly rather than through the library's dlopen wrapper, so it
 * can't use the deleters in ibv-ptr.h. */
struct VerbsDeleter {
    void operator()(struct ibv_device **list) const noexcept
    {
        ibv_free_device_list(list);
    }
    void operator()(struct ibv_context *ctx) const noexcept
    {
        (void)ibv_close_device(ctx);
    }
    void operator()(struct ibv_pd *pd) const noexcept
    {
        (void)ibv_dealloc_pd(pd);
    }
    void operator()(struct ibv_cq *cq) const noexcept
    {
        (void)ibv_destroy_cq(cq);
    }
    void operator()(struct ibv_qp *qp) const noexcept
    {
        (void)ibv_destroy_qp(qp);
    }
    void operator()(struct ibv_mr *mr) const noexcept
    {
        (void)ibv_dereg_mr(mr);
    }
};

template <typename T> using VerbsPtr = std::unique_ptr<T, VerbsDeleter>;

/* The verbs objects and transfer buffer for one transfer, released on
 * every return path. Work may still be posted when a transfer fails, so
 * the destructor destroys the QP before deregistering the MR. If either
 * step fails, the NIC may still reach the buffer, so the MR and buffer
 * are leaked instead of freed. The remaining members are released by
 * their deleters, in reverse declaration order. */
struct ClientVerbs {
    VerbsPtr<struct ibv_device *[]> devs{};
    VerbsPtr<struct ibv_context>    ctx{};
    VerbsPtr<struct ibv_pd>         pd{};
    VerbsPtr<struct ibv_cq>         cq{};
    hipObj::MallocPtr               buf{};
    VerbsPtr<struct ibv_mr>         mr{};
    VerbsPtr<struct ibv_qp>         qp{};

    ClientVerbs() = default;
    ~ClientVerbs()
    {
        struct ibv_qp *rawQp   = qp.release();
        bool           bufSafe = rawQp == nullptr || ibv_destroy_qp(rawQp) == 0;
        struct ibv_mr *rawMr   = mr.release();
        if (bufSafe && rawMr != nullptr) {
            bufSafe = ibv_dereg_mr(rawMr) == 0;
        }
        if (!bufSafe) {
            std::ignore = buf.release();
        }
    }

    ClientVerbs(const ClientVerbs &)            = delete;
    ClientVerbs &operator=(const ClientVerbs &) = delete;
    ClientVerbs(ClientVerbs &&)                 = delete;
    ClientVerbs &operator=(ClientVerbs &&)      = delete;
};

struct AddrInfoDeleter {
    void operator()(struct addrinfo *ai) const noexcept
    {
        freeaddrinfo(ai);
    }
};

using AddrInfoPtr = std::unique_ptr<struct addrinfo, AddrInfoDeleter>;

/* Returns a connected socket, or an empty UniqueFd on failure. Every
 * path returns fd, so the copy can be elided (-Wnrvo). */
hipObj::UniqueFd
connectTcp(const std::string &host, int port)
{
    hipObj::UniqueFd  fd;
    const std::string service = std::to_string(port);
    addrinfo          hints{};
    hints.ai_family         = AF_INET;
    hints.ai_socktype       = SOCK_STREAM;
    struct addrinfo *rawRes = nullptr;
    /* getaddrinfo() leaves rawRes unspecified when it fails */
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &rawRes) != 0) {
        return fd;
    }
    const AddrInfoPtr res(rawRes);
    if (res) {
        fd.reset(socket(res->ai_family, res->ai_socktype, 0));
        if (fd && connect(fd.get(), res->ai_addr, res->ai_addrlen) != 0) {
            fd.reset();
        }
    }
    return fd;
}

bool
sendAll(int fd, const std::string &s)
{
    size_t off = 0;
    while (off < s.size()) {
        const std::string_view rest = std::string_view(s).substr(off);
        ssize_t                n    = send(fd, rest.data(), rest.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string
readResponse(int fd)
{
    std::string out;
    char        buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) {
            /* headers complete and there is no body on control replies */
            break;
        }
    }
    return out;
}

int
statusOf(const std::string &resp)
{
    return hipobj::test::parseStatusCode(resp).value_or(0);
}

std::string
lowerCopy(std::string s)
{
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string
headerValue(const std::string &resp, const std::string &name)
{
    /* HTTP header names are case-insensitive; the server emits
     * X-Amz-... while callers ask for lowercase. */
    const std::string lower  = lowerCopy(resp);
    std::string       needle = lowerCopy(name) + ":";
    size_t            pos    = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        size_t lineStart = pos;
        /* match at line start */
        if (lineStart == 0 || lower[lineStart - 1] == '\n') {
            size_t vpos = pos + needle.size();
            while (vpos < lower.size() && lower[vpos] == ' ') {
                ++vpos;
            }
            size_t vend = lower.find("\r\n", vpos);
            return resp.substr(vpos, vend == std::string::npos ? std::string::npos : vend - vpos);
        }
        pos += needle.size();
    }
    return "";
}

/* One full transfer. Returns 0 on success. `op` is GET or PUT;
 * `size` bytes against the seeded object named by `target`. The
 * control exchange runs over TCP; the data phase uses verbs with
 * whatever device the host has. */
int
runTransfer(const std::string &host, int port, const std::string &op, const std::string &target,
            uint64_t size, uint32_t cookie)
{
    /* ---- verbs setup (client side) ---- */
    ClientVerbs verbs;
    int         n = 0;
    verbs.devs.reset(ibv_get_device_list(&n));
    if (verbs.devs == nullptr || n == 0) {
        std::fprintf(stderr, "dp: no verbs device\n");
        return 2;
    }
    verbs.ctx.reset(ibv_open_device(verbs.devs[0]));
    verbs.pd.reset(ibv_alloc_pd(verbs.ctx.get()));
    verbs.cq.reset(ibv_create_cq(verbs.ctx.get(), 16, nullptr, nullptr, 0));
    ibv_qp_init_attr init{};
    init.qp_type          = IBV_QPT_RC;
    init.send_cq          = verbs.cq.get();
    init.recv_cq          = verbs.cq.get();
    init.cap.max_send_wr  = 8;
    init.cap.max_recv_wr  = 8;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    verbs.qp.reset(ibv_create_qp(verbs.pd.get(), &init));
    if (!verbs.ctx || !verbs.pd || !verbs.cq || !verbs.qp) {
        std::fprintf(stderr, "dp: verbs setup failed\n");
        return 2;
    }

    /* Page-aligned transfer buffer: the emulated NIC maps user MRs
     * page by page and rejects a region whose chunk count does not
     * divide the length, which a stack buffer crossing a page
     * boundary triggers. */
    verbs.buf.reset(std::aligned_alloc(kBufBytes, kBufBytes));
    if (verbs.buf == nullptr) {
        std::fprintf(stderr, "dp: alloc failed\n");
        return 2;
    }
    const std::span<char> buf = hipObj::spanFromPointer(static_cast<char *>(verbs.buf.get()), kBufBytes);
    verbs.mr.reset(ibv_reg_mr(verbs.pd.get(), buf.data(), buf.size(),
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
    if (!verbs.mr) {
        std::fprintf(stderr, "dp: reg_mr failed\n");
        return 2;
    }
    std::ranges::fill(buf, '\0');
    if (op == "PUT") {
        std::ranges::copy(std::string_view("dp-put-payload"), buf.begin());
    }

    ibv_qp_attr attr{};
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(verbs.qp.get(), &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        std::fprintf(stderr, "dp: QP to INIT failed\n");
        return 2;
    }

    ibv_gid gid{};
    if (ibv_query_gid(verbs.ctx.get(), 1, 0, &gid) != 0) {
        std::fprintf(stderr, "dp: query_gid failed\n");
        return 2;
    }

    /* ---- PREPARE (control, over TCP) ---- */
    const std::string psnHex    = hipObj::formatHex(1U, 6);
    const std::string cookieHex = hipObj::formatHex(cookie, 8);
    const std::string mrAddrHex = hipObj::formatHex(reinterpret_cast<uintptr_t>(buf.data()));
    const std::string mrRkeyHex = hipObj::formatHex(verbs.mr->rkey);
    const std::string sizeDec   = std::to_string(size);

    hipObj::v2::BuiltinVerifier signer("hipobj-test-key", "hipobj-test-secret", "us-east-1");
    char                        amzDate[24];
    {
        std::time_t t = std::time(nullptr);
        struct tm   tmv;
        gmtime_r(&t, &tmv);
        std::strftime(amzDate, sizeof(amzDate), "%Y%m%dT%H%M%SZ", &tmv);
    }
    const std::string qpnHex = hipObj::formatHex(verbs.qp->qp_num);

    /* Real peer token: this client's QPN and GID so the server
     * pairs back through the token-carried endpoint. ibv-gid.h's
     * conversions need the library's ibv-core.h, which conflicts with
     * the real verbs.h this client uses, so convert with std::bit_cast. */
    hipObj::RdmaToken clientTok{};
    clientTok.qpNum                = verbs.qp->qp_num;
    clientTok.gid                  = std::bit_cast<hipObj::Gid>(gid);
    clientTok.transport            = hipObj::TransportType::RC;
    clientTok.portNum              = 1;
    const std::string clientTokHex = hipObj::encodeRdmaToken(clientTok);

    std::map<std::string, std::string> phdrs = {
        {"host", host},
        {"x-amz-date", amzDate},
        {"x-amz-rdma-protocol", "hipobj-rc-v2"},
        {"x-amz-rdma-token", clientTokHex},
        {"x-amz-rdma-psn", psnHex},
        {"x-amz-rdma-cookie", cookieHex},
        {"x-amz-rdma-op", op},
        {"x-amz-rdma-target", target},
        {"x-amz-rdma-size", sizeDec},
        {"x-amz-rdma-mr-addr", mrAddrHex},
        {"x-amz-rdma-mr-rkey", mrRkeyHex},
        {"x-amz-rdma-qpn", qpnHex},
    };
    std::string pauth = signer.sign("POST", "/.hipobj-rc/prepare", phdrs, "", amzDate);

    std::string prep = std::string("POST /.hipobj-rc/prepare HTTP/1.1\r\n");
    for (const auto &kv : phdrs) {
        prep += kv.first + ": " + kv.second + "\r\n";
    }
    prep += "Authorization: " + pauth + "\r\n";
    prep += "content-length: 0\r\n\r\n";

    hipObj::UniqueFd fd = connectTcp(host, port);
    if (!fd || !sendAll(fd.get(), prep)) {
        std::fprintf(stderr, "dp: prepare send failed\n");
        return 2;
    }
    std::string presp = readResponse(fd.get());
    fd.reset();
    int st = statusOf(presp);
    if (st != 200) {
        std::fprintf(stderr, "dp: PREPARE status=%d\n", st);
        return 1;
    }
    std::string session = headerValue(presp, "x-amz-rdma-session");
    std::string sqpnS   = headerValue(presp, "x-amz-rdma-qpn");
    /* The data E2E is meaningless without a server transport: a
     * missing QPN means the control plane answered without a QP
     * (device-less host), which must fail loudly here. */
    if (sqpnS.empty()) {
        std::fprintf(stderr, "dp: no server qpn exposed - refusing\n");
        return 1;
    }
    const std::optional<uint32_t> sqpnParsed = hipObj::parseNumber<uint32_t>(sqpnS, 16);
    if (!sqpnParsed || *sqpnParsed == 0 || *sqpnParsed > kMax24Bit) {
        std::fprintf(stderr, "dp: server qpn invalid\n");
        return 1;
    }
    const uint32_t    sqpn  = *sqpnParsed;
    const std::string spsnS = headerValue(presp, "x-amz-rdma-psn");

    hipObj::RdmaToken replyTok{};
    ibv_gid           serverGid{};
    /* The reply token is mandatory: "200:" + exactly 88 hex that
     * decodes and carries the same server QPN as the dedicated
     * header. Its absence or any malformation fails the transfer. */
    {
        std::string sreply = headerValue(presp, "x-amz-rdma-reply");
        size_t      cpos   = sreply.find(':');
        if (cpos == std::string::npos || sreply.substr(0, cpos) != "200") {
            std::fprintf(stderr, "dp: reply token missing/invalid code\n");
            return 1;
        }
        std::string tokHex  = sreply.substr(cpos + 1);
        const auto  decoded = hipObj::decodeRdmaTokenHex(tokHex);
        if (!decoded) {
            std::fprintf(stderr, "dp: reply token does not decode\n");
            return 1;
        }
        replyTok = *decoded;
        if (replyTok.qpNum != sqpn) {
            std::fprintf(stderr, "dp: reply token qpn mismatch\n");
            return 1;
        }
        /* Route by the server GID the token carries; RC transport with
         * a real GID is mandatory. */
        if (replyTok.transport != hipObj::TransportType::RC || replyTok.gid == hipObj::Gid{}) {
            std::fprintf(stderr, "dp: reply token unusable endpoint\n");
            return 1;
        }
        serverGid = std::bit_cast<union ibv_gid>(replyTok.gid);
    }
    const std::optional<uint32_t> spsnParsed =
        spsnS.size() == kPsnHexLen ? hipObj::parseNumber<uint32_t>(spsnS, 16) : std::nullopt;
    if (!spsnParsed || *spsnParsed == 0 || *spsnParsed > kMax24Bit) {
        std::fprintf(stderr, "dp: server psn invalid\n");
        return 1;
    }
    const uint32_t spsn = *spsnParsed;
    if (session.empty()) {
        std::fprintf(stderr, "dp: no session in the PREPARE reply\n");
        return 1;
    }

    /* ---- QP to RTR/RTS against the server QP ---- */
    attr                    = {};
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = IBV_MTU_1024;
    attr.dest_qp_num        = sqpn;
    attr.rq_psn             = spsn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;
    /* hipObject targets RoCEv2: the GRH with the server's GID from
     * the reply token is the only routing path. */
    attr.ah_attr.is_global      = 1;
    attr.ah_attr.grh.dgid       = serverGid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit  = 1;
    attr.ah_attr.port_num       = 1;
    if (ibv_modify_qp(verbs.qp.get(), &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
        std::fprintf(stderr, "dp: QP to RTR failed\n");
        return 2;
    }
    attr               = {};
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 1;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(verbs.qp.get(), &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
        std::fprintf(stderr, "dp: QP to RTS failed\n");
        return 2;
    }

    /* ---- READY ---- */
    std::map<std::string, std::string> rhdrs = {
        {"host", host},
        {"x-amz-date", amzDate},
        {"x-amz-rdma-protocol", "hipobj-rc-v2"},
        {"x-amz-rdma-session", session},
        {"x-amz-rdma-cookie", cookieHex},
        {"x-amz-rdma-mr-addr", mrAddrHex},
        {"x-amz-rdma-mr-rkey", mrRkeyHex},
        {"x-amz-rdma-qpn", qpnHex},
    };
    std::string rauth = signer.sign("POST", "/.hipobj-rc/ready", rhdrs, "", amzDate);
    std::string ready = std::string("POST /.hipobj-rc/ready HTTP/1.1\r\n");
    for (const auto &kv : rhdrs) {
        ready += kv.first + ": " + kv.second + "\r\n";
    }
    ready += "Authorization: " + rauth + "\r\n";
    ready += "content-length: 0\r\n\r\n";
    fd = connectTcp(host, port);
    if (!fd || !sendAll(fd.get(), ready)) {
        std::fprintf(stderr, "dp: ready send failed\n");
        return 2;
    }

    /* ---- data plane while the server processes READY ---- */
    if (op == "GET") {
        ibv_sge rsge{};
        rsge.addr   = reinterpret_cast<uintptr_t>(buf.data());
        rsge.length = static_cast<uint32_t>(std::min<uint64_t>(size, kBufBytes));
        rsge.lkey   = verbs.mr->lkey;
        ibv_recv_wr rwr{};
        rwr.wr_id                = 0x47455452454356ULL; /* "GETRECV" */
        rwr.sg_list              = &rsge;
        rwr.num_sge              = 1;
        struct ibv_recv_wr *rbad = nullptr;
        if (ibv_post_recv(verbs.qp.get(), &rwr, &rbad) != 0) {
            std::fprintf(stderr, "dp: GET post_recv failed\n");
            return 1;
        }
    }
    /* PUT: the server posts its receive while handling the READY
     * request and only answers after the transfer completes, so the
     * write must go out before waiting for the response. A short
     * grace keeps it clear of the receive-post race. */
    int rc = 0;
    if (op == "PUT") {
        usleep(100 * 1000);
        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uintptr_t>(buf.data());
        sge.length = static_cast<uint32_t>(std::min<uint64_t>(size, kBufBytes));
        sge.lkey   = verbs.mr->lkey;
        ibv_send_wr wr{};
        wr.opcode     = IBV_WR_RDMA_WRITE_WITH_IMM;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.imm_data   = htonl(cookie);
        /* Remote endpoint: the server staging MR from the PREPARE
         * reply. */
        const std::optional<uint64_t> saddr =
            hipObj::parseNumber<uint64_t>(headerValue(presp, "x-amz-rdma-mr-addr"), 16);
        const std::optional<uint32_t> srkey =
            hipObj::parseNumber<uint32_t>(headerValue(presp, "x-amz-rdma-mr-rkey"), 16);
        /* The write must not run past the end of the address space */
        if (saddr && srkey && *saddr <= std::numeric_limits<uint64_t>::max() - sge.length) {
            wr.wr.rdma.remote_addr  = *saddr;
            wr.wr.rdma.rkey         = *srkey;
            wr.sg_list              = &sge;
            wr.num_sge              = 1;
            struct ibv_send_wr *bad = nullptr;
            if (ibv_post_send(verbs.qp.get(), &wr, &bad) != 0) {
                std::fprintf(stderr, "dp: post WRITE_WITH_IMM failed\n");
                rc = 1;
            }
            else {
                ibv_wc wc{};
                for (int i = 0; i < 2000; ++i) {
                    if (ibv_poll_cq(verbs.cq.get(), 1, &wc) > 0) {
                        break;
                    }
                    usleep(1000);
                }
                if (wc.status != IBV_WC_SUCCESS) {
                    std::fprintf(stderr, "dp: PUT completion status=%d\n", static_cast<int>(wc.status));
                    rc = 1;
                }
                else {
                    std::printf("dp: PUT ok cookie=%08" PRIx32 " posted\n", cookie);
                }
            }
        }
        else {
            std::fprintf(stderr, "dp: server staging not exposed or invalid\n");
            rc = 1;
        }
    }
    std::string rresp = readResponse(fd.get());
    fd.reset();
    int         rst    = statusOf(rresp);
    std::string bytesS = headerValue(rresp, "x-amz-rdma-bytes-transferred");
    std::printf("dp: READY status=%d bytes=%s\n", rst, bytesS.c_str());
    if (rst != 200) {
        return 1;
    }
    if (op == "GET") {
        ibv_wc wc{};
        for (int i = 0; i < 2000; ++i) {
            if (ibv_poll_cq(verbs.cq.get(), 1, &wc) > 0) {
                break;
            }
            usleep(1000);
        }
        if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM &&
            (wc.wc_flags & IBV_WC_WITH_IMM) != 0 && ntohl(wc.imm_data) == cookie) {
            /* The GET must return the object bytes, not just a
             * completion: verify the seeded marker arrived. */
            const std::string expect = "dp-put-p";
            const std::string got(hipObj::asStringView(buf.first(expect.size())));
            if (got == expect) {
                std::printf("dp: GET ok cookie echoed, payload verified\n");
            }
            else {
                std::fprintf(stderr, "dp: GET payload mismatch: got %s want %s\n", got.c_str(),
                             expect.c_str());
                rc = 1;
            }
        }
        else {
            std::fprintf(stderr,
                         "dp: GET completion bad (status=%d op=%d "
                         "imm=%08" PRIx32 ")\n",
                         static_cast<int>(wc.status), static_cast<int>(wc.opcode), ntohl(wc.imm_data));
            rc = 1;
        }
    }

    return rc;
}

} // namespace

int
main(int argc, char *argv[])
{
    const std::span<char *>        argvSpan = hipObj::argSpan(argc, argv);
    const std::vector<std::string> args(argvSpan.begin(), argvSpan.end());
    if (args.size() < 6) {
        const std::string prog = args.empty() ? "v2-data-client" : args[0];
        std::fprintf(stderr, "usage: %s HOST PORT OP TARGET SIZE [COOKIE-HEX]\n", prog.c_str());
        return 2;
    }
    const std::optional<uint16_t> port = hipObj::parseNumber<uint16_t>(args[2]);
    if (!port || *port == 0) {
        std::fprintf(stderr, "invalid port %s\n", args[2].c_str());
        return 2;
    }
    const std::optional<uint64_t> size = hipObj::parseNumber<uint64_t>(args[5]);
    if (!size || *size == 0) {
        std::fprintf(stderr, "invalid size %s\n", args[5].c_str());
        return 2;
    }
    uint32_t cookie = 0x1a2b3c4d;
    if (args.size() > 6) {
        const std::optional<uint32_t> parsed = hipObj::parseNumber<uint32_t>(args[6], 16);
        if (!parsed) {
            std::fprintf(stderr, "invalid cookie %s\n", args[6].c_str());
            return 2;
        }
        cookie = *parsed;
    }
    return runTransfer(args[1], *port, args[3], args[4], *size, cookie);
}
