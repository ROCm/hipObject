/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal v2 control-protocol client harness. Signs requests with
 * the shared SigV4 helper and drives PREPARE/READY/CANCEL against
 * the reference server over plain TCP; exit codes encode the
 * outcome for the E2E script.
 *
 *   v2-client-harness <endpoint-host> <port> <op> <target> <size>
 *     [--cookie-hex N] [--expect-status N] [--cancel-after]
 *
 * The data phase (RDMA) is exercised by the transport layer; this
 * harness validates the control semantics: session creation, cookie
 * echo, error statuses, cancel idempotency. */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "v2_sigv4.h"

namespace {

int connectTo(const std::string& host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool sendAll(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

/* Reads until the header terminator; returns the raw response. */
std::string readResponse(int fd) {
  std::string raw;
  char buf[4096];
  while (raw.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    raw.append(buf, static_cast<size_t>(n));
  }
  return raw;
}

int statusOf(const std::string& raw) {
  /* "HTTP/1.1 NNN ..." */
  size_t sp = raw.find(' ');
  if (sp == std::string::npos) {
    return -1;
  }
  return std::atoi(raw.c_str() + sp + 1);
}

std::string headerValue(const std::string& raw, const char* name) {
  std::string lower;
  lower.reserve(raw.size());
  for (char c : raw) {
    lower.push_back(
      static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  const std::string needle = std::string("\n") + name + ":";
  size_t pos = lower.find(needle);
  if (pos == std::string::npos) {
    return std::string();
  }
  size_t start = pos + needle.size() + 1;
  size_t end = lower.find("\r\n", start);
  std::string v = raw.substr(start, end == std::string::npos ? std::string::npos
                                                             : end - start);
  /* trim */
  size_t b = v.find_first_not_of(" \t");
  size_t e = v.find_last_not_of(" \t\r");
  if (b == std::string::npos) {
    return std::string();
  }
  return v.substr(b, e - b + 1);
}

std::string hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return buf;
}

std::string hex24(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%06x", v);
  return buf;
}

/* Current UTC time in SigV4 format. */
std::string nowAmzDate() {
  std::time_t t = std::time(nullptr);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[24];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tmv);
  return buf;
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 6) {
    fprintf(stderr,
            "usage: %s host port op target size [--expect N] "
            "[--cookie-hex N] [--cancel]\n",
            argv[0]);
    return 2;
  }
  const std::string host = argv[1];
  const int port = std::atoi(argv[2]);
  const std::string op = argv[3];
  const std::string target = argv[4];
  const uint64_t size = std::strtoull(argv[5], nullptr, 10);
  int expect = 200;
  uint32_t cookie = 0x11223344;
  uint32_t readyCookie = 0;
  bool overrideReadyCookie = false;
  bool doCancel = false;
  for (int i = 6; i < argc; ++i) {
    if (std::strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
      expect = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--cookie-hex") == 0 && i + 1 < argc) {
      cookie = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
    } else if (std::strcmp(argv[i], "--ready-cookie-hex") == 0 &&
               i + 1 < argc) {
      readyCookie = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
      overrideReadyCookie = true;
    } else if (std::strcmp(argv[i], "--cancel") == 0) {
      doCancel = true;
    }
  }

  hipObj::v2::BuiltinVerifier signer("hipobj-test-key", "hipobj-test-secret",
                                     "us-east-1");
  const std::string amzDate = nowAmzDate();

  /* --- PREPARE --- */
  std::map<std::string, std::string> hdrs = {
    {"host", host + ":" + std::to_string(port)},
    {"x-amz-date", amzDate},
    {"x-amz-rdma-protocol", "hipobj-rc-v2"},
    {"x-amz-rdma-token", std::string(88, '1')},
    {"x-amz-rdma-psn", "00a1b2c"},
    {"x-amz-rdma-cookie", hex32(cookie)},
    {"x-amz-rdma-op", op},
    {"x-amz-rdma-target", target},
    {"x-amz-rdma-size", std::to_string(size)},
  };
  /* Client PSN must be 6 hex digits: 0x0a1b2c. */
  hdrs["x-amz-rdma-psn"] = "0a1b2c";
  std::string auth = signer.sign("POST", "/.hipobj-rc/prepare", hdrs, "",
                                 amzDate);

  std::string req = "POST /.hipobj-rc/prepare HTTP/1.1\r\n";
  for (const auto& [k, v] : hdrs) {
    req += k + ": " + v + "\r\n";
  }
  req += "Authorization: " + auth + "\r\n";
  req += "Content-Length: 0\r\n\r\n";

  int fd = connectTo(host, port);
  if (fd < 0) {
    fprintf(stderr, "harness: connect failed\n");
    return 2;
  }
  if (!sendAll(fd, req)) {
    fprintf(stderr, "harness: prepare send failed\n");
    close(fd);
    return 2;
  }
  std::string resp = readResponse(fd);
  close(fd);
  int prepStatus = statusOf(resp);
  if (prepStatus != 200) {
    fprintf(stdout, "PREPARE status=%d\n", prepStatus);
    return prepStatus == expect ? 0 : 1;
  }
  std::string session = headerValue(resp, "x-amz-rdma-session");

  /* --- READY --- */
  std::map<std::string, std::string> rhdrs = {
    {"host", host + ":" + std::to_string(port)},
    {"x-amz-date", amzDate},
    {"x-amz-rdma-protocol", "hipobj-rc-v2"},
    {"x-amz-rdma-session", session},
    {"x-amz-rdma-cookie", hex32(overrideReadyCookie ? readyCookie : cookie)},
    /* Null data-plane endpoint: this harness drives the control
     * plane only. A server with transport objects treats a null
     * MR address as an unwired GET/PUT and the session-side
     * check handles it before the data phase. */
    {"x-amz-rdma-mr-addr", "0"},
    {"x-amz-rdma-mr-rkey", "0"},
  };
  std::string rauth = signer.sign("POST", "/.hipobj-rc/ready", rhdrs, "",
                                  amzDate);
  std::string rreq = "POST /.hipobj-rc/ready HTTP/1.1\r\n";
  for (const auto& [k, v] : rhdrs) {
    rreq += k + ": " + v + "\r\n";
  }
  rreq += "Authorization: " + rauth + "\r\n";
  rreq += "Content-Length: 0\r\n\r\n";

  fd = connectTo(host, port);
  if (fd < 0 || !sendAll(fd, rreq)) {
    fprintf(stderr, "harness: ready send failed\n");
    return 2;
  }
  resp = readResponse(fd);
  close(fd);
  int readyStatus = statusOf(resp);
  fprintf(stdout, "READY status=%d session=%.8s..\n", readyStatus,
          session.c_str());
  if (readyStatus != expect) {
    return 1;
  }
  /* Cookie echo must match on success. */
  if (expect == 200) {
    std::string echo = headerValue(resp, "x-amz-rdma-cookie");
    if (echo != hex32(cookie)) {
      fprintf(stderr, "harness: cookie echo mismatch (%s)\n", echo.c_str());
      return 1;
    }
  }

  /* --- optional CANCEL --- */
  if (doCancel) {
    std::map<std::string, std::string> chdrs = {
      {"host", host + ":" + std::to_string(port)},
      {"x-amz-date", amzDate},
      {"x-amz-rdma-protocol", "hipobj-rc-v2"},
      {"x-amz-rdma-session", session},
    };
    std::string cauth = signer.sign("POST", "/.hipobj-rc/cancel", chdrs, "",
                                    amzDate);
    std::string creq = "POST /.hipobj-rc/cancel HTTP/1.1\r\n";
    for (const auto& [k, v] : chdrs) {
      creq += k + ": " + v + "\r\n";
    }
    creq += "Authorization: " + cauth + "\r\n";
    creq += "Content-Length: 0\r\n\r\n";
    fd = connectTo(host, port);
    if (fd >= 0 && sendAll(fd, creq)) {
      resp = readResponse(fd);
      close(fd);
      int c1 = statusOf(resp);
      /* second cancel must also be 204 (idempotent) */
      fd = connectTo(host, port);
      if (fd >= 0 && sendAll(fd, creq)) {
        resp = readResponse(fd);
        close(fd);
        int c2 = statusOf(resp);
        fprintf(stdout, "CANCEL %d %d\n", c1, c2);
        if (c1 != 204 || c2 != 204) {
          return 1;
        }
      }
    }
  }
  return 0;
}
