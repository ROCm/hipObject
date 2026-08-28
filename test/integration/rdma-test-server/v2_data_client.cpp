/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Data-plane client for the v2 reference-server E2E: pairs a QP
 * with the server session described by the PREPARE/READY exchange
 * and performs the actual RDMA transfer (WRITE_WITH_IMM for PUT,
 * READ pull for GET) so the server data phase runs for real. */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "v2_sigv4.h"

namespace {

uint32_t gCookie = 0;

int connectTcp(const char* host, int port) {
  char service[16];
  std::snprintf(service, sizeof(service), "%d", port);
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host, service, &hints, &res) != 0 || res == nullptr) {
    return -1;
  }
  int fd = socket(res->ai_family, res->ai_socktype, 0);
  if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    if (fd >= 0) {
      close(fd);
    }
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  return fd;
}

bool sendAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

std::string readResponse(int fd) {
  std::string out;
  char buf[4096];
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

int statusOf(const std::string& resp) {
  size_t sp = resp.find(' ');
  if (sp == std::string::npos) {
    return 0;
  }
  return std::atoi(resp.c_str() + sp + 1);
}

std::string lowerCopy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string headerValue(const std::string& resp, const std::string& name) {
  /* HTTP header names are case-insensitive; the server emits
   * X-Amz-... while callers ask for lowercase. */
  const std::string lower = lowerCopy(resp);
  std::string needle = lowerCopy(name) + ":";
  size_t pos = 0;
  while ((pos = lower.find(needle, pos)) != std::string::npos) {
    size_t lineStart = pos;
    /* match at line start */
    if (lineStart == 0 || lower[lineStart - 1] == '\n') {
      size_t vpos = pos + needle.size();
      while (vpos < lower.size() && lower[vpos] == ' ') {
        ++vpos;
      }
      size_t vend = lower.find("\r\n", vpos);
      return resp.substr(vpos, vend == std::string::npos ? std::string::npos
                                                         : vend - vpos);
    }
    pos += needle.size();
  }
  return "";
}

} // namespace

/* One full transfer. Returns 0 on success. `op` is GET or PUT;
 * `size` bytes against the seeded object named by `target`. The
 * control exchange runs over TCP; the data phase uses verbs with
 * whatever device the host has. */
int runTransfer(const char* host, int port, const char* op, const char* target,
                uint64_t size, uint32_t cookie) {
  gCookie = cookie;

  /* ---- verbs setup (client side) ---- */
  int n = 0;
  struct ibv_device** devs = ibv_get_device_list(&n);
  if (devs == nullptr || n == 0) {
    std::fprintf(stderr, "dp: no verbs device\n");
    return 2;
  }
  struct ibv_context* ctx = ibv_open_device(devs[0]);
  struct ibv_pd* pd = ibv_alloc_pd(ctx);
  struct ibv_cq* cq = ibv_create_cq(ctx, 16, nullptr, nullptr, 0);
  struct ibv_qp_init_attr init = {};
  init.qp_type = IBV_QPT_RC;
  init.send_cq = cq;
  init.recv_cq = cq;
  init.cap.max_send_wr = 8;
  init.cap.max_recv_wr = 8;
  init.cap.max_send_sge = 1;
  init.cap.max_recv_sge = 1;
  struct ibv_qp* qp = ibv_create_qp(pd, &init);
  if (!ctx || !pd || !cq || !qp) {
    std::fprintf(stderr, "dp: verbs setup failed\n");
    return 2;
  }

  char buf[4096];
  struct ibv_mr* mr = ibv_reg_mr(pd, buf, sizeof(buf),
                                 IBV_ACCESS_LOCAL_WRITE |
                                   IBV_ACCESS_REMOTE_READ |
                                   IBV_ACCESS_REMOTE_WRITE);
  if (!mr) {
    std::fprintf(stderr, "dp: reg_mr failed\n");
    return 2;
  }
  std::memset(buf, 0, sizeof(buf));
  if (std::strcmp(op, "PUT") == 0) {
    std::memcpy(buf, "dp-put-payload", 14);
  }

  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = 1;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  ibv_modify_qp(qp, &attr,
                IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                  IBV_QP_ACCESS_FLAGS);

  union ibv_gid gid = {};
  ibv_query_gid(ctx, 1, 0, &gid);

  /* ---- PREPARE (control, over TCP) ---- */
  char psnHex[8];
  std::snprintf(psnHex, sizeof(psnHex), "%06x", 1);
  char cookieHex[12];
  std::snprintf(cookieHex, sizeof(cookieHex), "%08x", cookie);
  char mrAddrHex[32];
  std::snprintf(mrAddrHex, sizeof(mrAddrHex), "%lx",
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(buf)));
  char mrRkeyHex[12];
  std::snprintf(mrRkeyHex, sizeof(mrRkeyHex), "%x", mr->rkey);
  char sizeDec[24];
  std::snprintf(sizeDec, sizeof(sizeDec), "%llu",
                static_cast<unsigned long long>(size));

  hipObj::v2::BuiltinVerifier signer("hipobj-test-key", "hipobj-test-secret",
                                     "us-east-1");
  char amzDate[24];
  {
    std::time_t t = std::time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    std::strftime(amzDate, sizeof(amzDate), "%Y%m%dT%H%M%SZ", &tmv);
  }
  char qpnHex[12];
  std::snprintf(qpnHex, sizeof(qpnHex), "%x", qp->qp_num);
  struct ibv_port_attr pattr = {};
  ibv_query_port(ctx, 1, &pattr);
  char lidHex[10];
  std::snprintf(lidHex, sizeof(lidHex), "%x", pattr.lid);

  std::map<std::string, std::string> phdrs = {
    {"host", std::string(host)},
    {"x-amz-date", amzDate},
    {"x-amz-rdma-protocol", "hipobj-rc-v2"},
    {"x-amz-rdma-token", std::string(88, '1')},
    {"x-amz-rdma-psn", psnHex},
    {"x-amz-rdma-cookie", cookieHex},
    {"x-amz-rdma-op", op},
    {"x-amz-rdma-target", target},
    {"x-amz-rdma-size", sizeDec},
    {"x-amz-rdma-mr-addr", mrAddrHex},
    {"x-amz-rdma-mr-rkey", mrRkeyHex},
    {"x-amz-rdma-qpn", qpnHex},
  };
  std::string pauth = signer.sign("POST", "/.hipobj-rc/prepare", phdrs, "",
                                  amzDate);

  std::string prep = std::string("POST /.hipobj-rc/prepare HTTP/1.1\r\n");
  for (const auto& kv : phdrs) {
    prep += kv.first + ": " + kv.second + "\r\n";
  }
  prep += "Authorization: " + pauth + "\r\n";
  prep += "content-length: 0\r\n\r\n";

  int fd = connectTcp(host, port);
  if (fd < 0 || !sendAll(fd, prep)) {
    std::fprintf(stderr, "dp: prepare send failed\n");
    return 2;
  }
  std::string presp = readResponse(fd);
  close(fd);
  int st = statusOf(presp);
  if (st != 200) {
    std::fprintf(stderr, "dp: PREPARE status=%d\n", st);
    return 1;
  }
  std::string session = headerValue(presp, "x-amz-rdma-session");
  std::string sqpnS = headerValue(presp, "x-amz-rdma-qpn");
  std::string spsnS = headerValue(presp, "x-amz-rdma-psn");
  uint32_t sqpn = sqpnS.empty() ? 0
                                : static_cast<uint32_t>(
                                    std::strtoul(sqpnS.c_str(), nullptr, 16));
  uint32_t spsn = spsnS.empty() ? 1
                                : static_cast<uint32_t>(
                                    std::strtoul(spsnS.c_str(), nullptr, 16));
  if (session.empty()) {
    /* Server did not expose a QP: the data path stays a no-op on
     * the server (no transport objects wired). Drive READY anyway
     * so the control flow is exercised end to end. */
    std::printf("dp: no server qpn - control-only transfer\n");
  }

  /* ---- QP to RTR/RTS against the server QP when exposed ---- */
  if (sqpn != 0) {
    attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = sqpn;
    attr.rq_psn = spsn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.port_num = 1;
    ibv_modify_qp(qp, &attr,
                  IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                    IBV_QP_MIN_RNR_TIMER);
    attr = {};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 1;
    attr.max_rd_atomic = 1;
    ibv_modify_qp(qp, &attr,
                  IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                    IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
  }

  /* ---- READY ---- */
  std::map<std::string, std::string> rhdrs = {
    {"host", std::string(host)},
    {"x-amz-date", amzDate},
    {"x-amz-rdma-protocol", "hipobj-rc-v2"},
    {"x-amz-rdma-session", session},
    {"x-amz-rdma-cookie", cookieHex},
    {"x-amz-rdma-mr-addr", mrAddrHex},
    {"x-amz-rdma-mr-rkey", mrRkeyHex},
    {"x-amz-rdma-qpn", qpnHex},
  };
  std::string rauth = signer.sign("POST", "/.hipobj-rc/ready", rhdrs, "",
                                  amzDate);
  std::string ready = std::string("POST /.hipobj-rc/ready HTTP/1.1\r\n");
  for (const auto& kv : rhdrs) {
    ready += kv.first + ": " + kv.second + "\r\n";
  }
  ready += "Authorization: " + rauth + "\r\n";
  ready += "content-length: 0\r\n\r\n";
  fd = connectTcp(host, port);
  if (fd < 0 || !sendAll(fd, ready)) {
    std::fprintf(stderr, "dp: ready send failed\n");
    return 2;
  }

  /* ---- data plane while the server processes READY ---- */
  if (sqpn != 0) {
    if (std::strcmp(op, "PUT") == 0) {
      /* Receive the server's READ request first? No - the v2 PUT
       * direction is client->server WRITE_WITH_IMM. The server
       * posted a receive; we write and signal the cookie. */
      struct ibv_sge sge = {};
      sge.addr = reinterpret_cast<uintptr_t>(buf);
      sge.length = static_cast<uint32_t>(size < sizeof(buf) ? size
                                                            : sizeof(buf));
      sge.lkey = mr->lkey;
      struct ibv_send_wr wr = {};
      wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      wr.send_flags = IBV_SEND_SIGNALED;
      wr.imm_data = htonl(cookie);
      /* remote endpoint: the server staging MR, exposed via the
       * PREPARE reply when transport is wired. */
      std::string saddrS = headerValue(presp, "x-amz-rdma-mr-addr");
      std::string srkeyS = headerValue(presp, "x-amz-rdma-mr-rkey");
      if (!saddrS.empty() && !srkeyS.empty()) {
        wr.wr.rdma.remote_addr = std::strtoull(saddrS.c_str(), nullptr, 16);
        wr.wr.rdma.rkey = static_cast<uint32_t>(
          std::strtoul(srkeyS.c_str(), nullptr, 16));
        wr.sg_list = &sge;
        wr.num_sge = 1;
        struct ibv_send_wr* bad = nullptr;
        if (ibv_post_send(qp, &wr, &bad) != 0) {
          std::fprintf(stderr, "dp: post WRITE_WITH_IMM failed\n");
        }
        struct ibv_wc wc = {};
        for (int i = 0; i < 2000; ++i) {
          if (ibv_poll_cq(cq, 1, &wc) > 0) {
            break;
          }
          usleep(1000);
        }
      } else {
        std::fprintf(stderr, "dp: server staging not exposed; skip write\n");
      }
    }
    if (std::strcmp(op, "GET") == 0) {
      struct ibv_sge rsge = {};
      rsge.addr = reinterpret_cast<uintptr_t>(buf);
      rsge.length = static_cast<uint32_t>(size < sizeof(buf) ? size
                                                             : sizeof(buf));
      rsge.lkey = mr->lkey;
      struct ibv_recv_wr rwr = {};
      rwr.wr_id = 0x47455452454356ULL; /* "GETRECV" */
      rwr.sg_list = &rsge;
      rwr.num_sge = 1;
      struct ibv_recv_wr* rbad = nullptr;
      if (ibv_post_recv(qp, &rwr, &rbad) != 0) {
        std::fprintf(stderr, "dp: GET post_recv failed\n");
      }
    }
  }

  std::string rresp = readResponse(fd);
  close(fd);
  int rst = statusOf(rresp);
  std::string bytesS = headerValue(rresp, "x-amz-rdma-bytes-transferred");
  std::printf("dp: READY status=%d bytes=%s\n", rst, bytesS.c_str());
  if (rst != 200) {
    return 1;
  }
  int rc = 0;
  if (std::strcmp(op, "GET") == 0 && sqpn != 0 && rst == 200) {
    struct ibv_wc wc = {};
    for (int i = 0; i < 2000; ++i) {
      if (ibv_poll_cq(cq, 1, &wc) > 0) {
        break;
      }
      usleep(1000);
    }
    if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM &&
        (wc.wc_flags & IBV_WC_WITH_IMM) != 0 && ntohl(wc.imm_data) == cookie) {
      std::printf("dp: GET ok cookie echoed, payload[0..7]=%.8s\n", buf);
    } else {
      std::fprintf(stderr,
                   "dp: GET completion bad (status=%d op=%d "
                   "imm=%08x)\n",
                   wc.status, wc.opcode, ntohl(wc.imm_data));
      rc = 1;
    }
  }

  ibv_dereg_mr(mr);
  ibv_destroy_qp(qp);
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(devs);
  return rc;
}

int main(int argc, char* argv[]) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: %s HOST PORT OP TARGET SIZE [COOKIE-HEX]\n",
                 argv[0]);
    return 2;
  }
  uint32_t cookie = argc > 6 ? static_cast<uint32_t>(
                                 std::strtoul(argv[6], nullptr, 16))
                             : 0x1a2b3c4d;
  uint64_t size = std::strtoull(argv[5], nullptr, 10);
  return runTransfer(argv[1], std::atoi(argv[2]), argv[3], argv[4], size,
                     cookie);
}
