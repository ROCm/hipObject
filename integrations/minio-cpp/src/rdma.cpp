/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/rdma.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <net/if.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <hipobj.h>
#include <miniocpp/http.h>
#include <miniocpp/request.h>
#include <miniocpp/signer.h>
#include <miniocpp/utils.h>

namespace hipobj::minio {

namespace minio = ::minio;

namespace {

int parseRdmaReply(const std::string& rdma_reply) {
  if (rdma_reply.empty()) {
    return static_cast<int>(kRdmaNotSupported);
  }
  int httpCode = 0;
  hipObjError_t err = hipObjParseRdmaReply(rdma_reply.c_str(),
                                           rdma_reply.size(), &httpCode);
  if (err.opError != hipObjSuccess) {
    return 0;
  }
  if (httpCode == kRdmaReplyNotImplemented) {
    return static_cast<int>(kRdmaNotSupported);
  }
  return httpCode;
}

/* Canonical rdma-target fallback: /bucket/key with the sorted query
 * appended. The library normally supplies req->target already built;
 * this covers a null target (defensive) without a second encoder. */
std::string buildObjectTarget(const char* bucket, const char* key,
                              const char* query) {
  std::string t = std::string("/") + (bucket ? bucket : "") + "/" +
                  (key ? key : "");
  if (query && query[0] != '\0') {
    t += "?";
    t += query;
  }
  return t;
}

std::string clientNicFromToken(const char* token) {
  char nicIp[32];
  hipObjError_t err = hipObjTokenClientNic(token, nicIp, sizeof(nicIp));
  if (err.opError != hipObjSuccess || nicIp[0] == '\0') {
    return {};
  }
  return std::string(nicIp);
}

std::string clientNic() {
  /* The v2 stack owns NIC selection; ask it directly instead of
   * minting a v1 RDMA token (which needs a v1-registered buffer and
   * would admit unregistered memory into the transfer). */
  char* nic = hipObjNicV2();
  if (nic == nullptr) {
    return std::string();
  }
  const std::string out(nic);
  hipObjFreeNicV2(nic);
  return out;
}

// ---------------------------------------------------------------------------
// v2 callback context — carries the per-transfer minio credentials and
// endpoint so the three hipObjOpsV2_t callbacks can build signed requests.
// ---------------------------------------------------------------------------

/* One owned TCP connection for the v2 control plane. The split READY
 * exchange needs the request-write and the response-read as separate
 * steps over the same socket, which the SDK's single-call Execute()
 * cannot express. PREPARE and CANCEL are complete exchanges and use
 * the same socket sequentially. */
struct ControlConn {
  int fd = -1;
  std::string host;
  std::string port;

  bool connectTo(const std::string& host_port, const std::string& nic,
                 int deadlineMs);
  /* Absolute-deadline variants: one caller-computed callback deadline
   * is shared across connect, send, and read so the steps cannot
   * collectively exceed the reported budget. */
  bool connectToUntil(const std::string& host_port, const std::string& nic,
                      std::chrono::steady_clock::time_point deadlineAt);
  void close() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  bool sendAll(const std::string& bytes, int deadlineMs);
  bool sendAllUntil(
    const std::string& bytes,
    std::chrono::steady_clock::time_point deadlineAt);
  /* Reads one full CRLF-delimited HTTP/1.1 response: status line,
   * headers, then exactly Content-Length body bytes. Fails on chunked
   * or missing length (the control plane never uses them). */
  bool readResponse(std::string& head, std::string& body, int deadlineMs);
  bool readResponseUntil(
    std::string& head, std::string& body,
    std::chrono::steady_clock::time_point deadlineAt);
  ~ControlConn() { close(); }
};

struct V2CallbackCtx {
  S3RdmaContext* sctx;
  std::string clientNic; // NIC the v2 stack selected (hipObjNicV2)
  /* Split READY exchange state: the request was written and the socket
   * is parked until finishReady reads the response. */
  ControlConn readyConn;
  bool readyPending = false;
};

// hipObjOpsV2_t callbacks -------------------------------------------------

/* Parses "host[:port]" out of the S3 URL for a direct connection. */
bool ControlConn::connectTo(const std::string& host_port,
                            const std::string& nic, int deadlineMs) {
  const auto deadlineAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(deadlineMs);
  return connectToUntil(host_port, nic, deadlineAt);
}

bool ControlConn::connectToUntil(
  const std::string& host_port, const std::string& nic,
  std::chrono::steady_clock::time_point deadlineAt) {
  close();
  std::string host = host_port;
  std::string port = "80";
  bool https = false;
  size_t scheme = host_port.find("://");
  if (scheme != std::string::npos) {
    https = host_port.compare(0, scheme, "https") == 0;
    host = host_port.substr(scheme + 3);
  }
  size_t slash = host.find('/');
  if (slash != std::string::npos) host = host.substr(0, slash);
  size_t colon = host.rfind(':');
  if (colon != std::string::npos) {
    port = host.substr(colon + 1);
    host = host.substr(0, colon);
  }
  if (https) {
    /* TLS termination is out of scope for the direct control path:
     * the lab deployment serves the control plane over plain HTTP. */
    return false;
  }

  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* list = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0 ||
      list == nullptr) {
    return false;
  }
  int fd = -1;
  for (struct addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (!nic.empty()) {
      /* Bind the outbound socket to the RDMA NIC's interface so the
       * control traffic shares its fate with the data plane. */
      struct ifreq ifr{};
      std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", nic.c_str());
      if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr,
                     sizeof(ifr)) != 0) {
        /* Fall back to the interface's address when SO_BINDTODEVICE
         * needs privileges. */
        struct ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) == 0) {
          for (struct ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
            if (std::strcmp(i->ifa_name, nic.c_str()) != 0 ||
                i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) {
              continue;
            }
            sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
            if (bind(fd, reinterpret_cast<sockaddr*>(sa), sizeof(*sa)) == 0) {
              break;
            }
          }
          freeifaddrs(ifs);
        }
      }
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) break;
    if (errno == EINPROGRESS) {
      struct pollfd pfd{fd, POLLOUT, 0};
      int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadlineAt - std::chrono::steady_clock::now())
          .count());
      const int pr = poll(&pfd, 1, left);
      if (left <= 0 || (pr < 0 && errno != EINTR) || pr == 0) {
        ::close(fd);
        fd = -1;
        if (pr < 0 && errno == EINTR) {
          --ai; /* interrupted: retry this address within the budget */
          continue;
        }
        continue;
      }
      int err = 0;
      socklen_t elen = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
      if (err == 0) break;
    }
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(list);
  if (fd < 0) return false;
  /* Keep the socket nonblocking: sendAll/readResponse drive every
   * byte through poll, so a stalled peer cannot block past the
   * deadline. */
  this->fd = fd;
  this->host = host;
  this->port = port;
  return true;
}

bool ControlConn::sendAll(const std::string& bytes, int deadlineMs) {
  if (fd < 0) return false;
  const auto deadlineAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(deadlineMs);
  return sendAllUntil(bytes, deadlineAt);
}

/* Deadline-absolute variant: the caller computes one callback-wide
 * deadline and shares it across connect, send, and read so the sum of
 * every step stays inside the reported budget instead of each step
 * re-arming a fresh allowance. */
bool ControlConn::sendAllUntil(const std::string& bytes,
                               std::chrono::steady_clock::time_point
                                 deadlineAt) {
  if (fd < 0) return false;
  size_t off = 0;
  while (off < bytes.size()) {
    struct pollfd pfd{fd, POLLOUT, 0};
    int left = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        deadlineAt - std::chrono::steady_clock::now())
        .count());
    const int pr = poll(&pfd, 1, left);
    if (left <= 0 || (pr < 0 && errno != EINTR) || pr == 0)
      return false;
    ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        continue; /* poll said writable; retry the partial window */
      }
      return false;
    }
    if (n == 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool ControlConn::readResponse(std::string& head, std::string& body,
                               int deadlineMs) {
  if (fd < 0) return false;
  const auto deadlineAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(deadlineMs);
  return readResponseUntil(head, body, deadlineAt);
}

bool ControlConn::readResponseUntil(
  std::string& head, std::string& body,
  std::chrono::steady_clock::time_point deadlineAt) {
  if (fd < 0) return false;
  std::string buf;
  size_t headEnd = std::string::npos;
  size_t contentLen = std::string::npos;
  while (true) {
    if (headEnd == std::string::npos) {
      headEnd = buf.find("\r\n\r\n");
      if (headEnd != std::string::npos) {
        head = buf.substr(0, headEnd);
        const std::string rest = buf.substr(headEnd + 4);
        buf = rest;
        /* Framing contract for this client: exactly one
         * Content-Length, no Transfer-Encoding, no interim 1xx. The
         * reference server always sends Content-Length; anything else
         * is rejected rather than misread as an empty body. */
        size_t pos = 0;
        bool haveLen = false;
        bool chunked = false;
        while (pos < head.size()) {
          size_t eol = head.find("\r\n", pos);
          if (eol == std::string::npos) eol = head.size();
          std::string line = head.substr(pos, eol - pos);
          pos = (eol == head.size()) ? head.size() : eol + 2;
          const std::string needle = "content-length:";
          if (line.size() >= needle.size() &&
              strncasecmp(line.c_str(), needle.c_str(), needle.size()) == 0) {
            if (haveLen) return false; /* duplicate */
            haveLen = true;
            const std::string num =
              line.substr(needle.size());
            /* Trim surrounding whitespace (SP/HTAB), then require at
             * least one decimal digit and nothing else: signs,
             * prefixes, embedded spaces, or trailing garbage are
             * framing errors, not zero. */
            const size_t first =
              num.find_first_not_of(" \t");
            const size_t last =
              num.find_last_not_of(" \t");
            if (first == std::string::npos) {
              return false; /* empty or all-whitespace */
            }
            const std::string digits =
              num.substr(first, last - first + 1);
            if (digits.find_first_not_of("0123456789") !=
                  std::string::npos ||
                digits.size() > 20) {
              return false;
            }
            errno = 0;
            char* endp = nullptr;
            const unsigned long long v =
              std::strtoull(digits.c_str(), &endp, 10);
            if (errno != 0 || endp == nullptr ||
                *endp != '\0') {
              return false;
            }
            contentLen = static_cast<size_t>(v);
          }
          const std::string te = "transfer-encoding:";
          if (line.size() >= te.size() &&
              strncasecmp(line.c_str(), te.c_str(), te.size()) == 0) {
            chunked = true;
          }
        }
        if (chunked) return false;
        if (!haveLen) {
          /* No length at all is only acceptable for a no-body status
           * or HEAD-like replies; this client never sends those, so
           * treat it as malformed framing. */
          return false;
        }
      }
    }
    if (headEnd != std::string::npos && contentLen != std::string::npos &&
        buf.size() >= contentLen) {
      body = buf.substr(0, contentLen);
      return true;
    }
    struct pollfd pfd{fd, POLLIN, 0};
    int left = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        deadlineAt - std::chrono::steady_clock::now())
        .count());
    const int pr = poll(&pfd, 1, left);
    if (left <= 0 || (pr < 0 && errno != EINTR) || pr == 0)
      return false;
    char chunk[4096];
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) return false; /* peer closed mid-response */
    buf.append(chunk, static_cast<size_t>(n));
    if (buf.size() > (1u << 20)) return false; /* bounded */
  }
}

/* Builds and signs a control-plane request against the S3 endpoint,
 * serializes it as HTTP/1.1, and executes it over the supplied owned
 * connection. The control path lives under /.hipobj-rc/{prepare,ready,
 * cancel}; the canonical target (object path + sorted query) travels
 * in a header so the signed path stays the fixed control path. */
struct ControlExchange {
  std::string head;   /* status line + headers, CRLF lines */
  std::string bytes;  /* full serialized request */
};

bool buildControlExchange(ControlExchange& out, S3RdmaContext* sctx,
                          const std::string& control_path,
                          const std::string& nic,
                          minio::utils::Multimap& extra_headers,
                          const std::string& hostHeaderValue) {
  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::utils::Multimap sign_headers;
  sign_headers.Add("Host", hostHeaderValue);
  sign_headers.Add("x-amz-date", date.ToAmzDate());
  sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
  sign_headers.Add("Content-Length", "0");
  sign_headers.AddAll(extra_headers);
  if (!creds.session_token.empty()) {
    sign_headers.Add("X-Amz-Security-Token", creds.session_token);
  }
  minio::signer::SignV4S3(minio::http::Method::kPost, control_path,
                          sctx->region, sign_headers, query_params,
                          creds.access_key, creds.secret_key,
                          kUnsignedPayload, date);
  std::string req = "POST ";
  req += control_path;
  req += " HTTP/1.1\r\n";
  for (const std::string& h : sign_headers.ToHttpHeaders()) {
    req += h;
    req += "\r\n";
  }
  req += "Connection: keep-alive\r\n\r\n";
  out.bytes = req;
  return true;
}

/* Header block lookup helper over the raw CRLF response head. */
std::string headerValue(const std::string& head, const std::string& name) {
  size_t pos = 0;
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) {
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  while (pos < head.size()) {
    size_t eol = head.find("\r\n", pos);
    if (eol == std::string::npos) eol = head.size();
    std::string line = head.substr(pos, eol - pos);
    pos = (eol == head.size()) ? head.size() : eol + 2;
    if (line.size() > lower.size() + 1 &&
        strncasecmp(line.c_str(), lower.c_str(), lower.size()) == 0 &&
        line[lower.size()] == ':') {
      size_t vs = lower.size() + 1;
      while (vs < line.size() && line[vs] == ' ') ++vs;
      return line.substr(vs);
    }
  }
  return std::string();
}

int statusCodeFromHead(const std::string& head) {
  /* "HTTP/1.1 200 OK" */
  size_t sp = head.find(' ');
  if (sp == std::string::npos) return 0;
  return static_cast<int>(
    std::strtol(head.c_str() + sp + 1, nullptr, 10));
}

uint32_t hexToU32(const std::string& v) {
  /* The cookie echo is exactly eight hex digits on the wire. */
  if (v.size() != 8) return 0;
  for (char ch : v) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return 0;
  }
  char* endp = nullptr;
  unsigned long n = std::strtoul(v.c_str(), &endp, 16);
  if (endp == nullptr || *endp != '\0' || n > 0xffffffffUL) return 0;
  return static_cast<uint32_t>(n);
}

std::string hex32Bridge(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return std::string(buf);
}

std::string hex24Bridge(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%06x", v);
  return std::string(buf);
}

std::string hex64Bridge(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llx",
                static_cast<unsigned long long>(v));
  return std::string(buf);
}

/* The PREPARE reply token arrives as "200:<88-hex>" in
 * X-Amz-Rdma-Reply; strip the status prefix and keep the payload. */
std::string replyTokenPayload(const std::string& v) {
  const std::string prefix = "200:";
  if (v.size() > prefix.size() &&
      v.compare(0, prefix.size(), prefix) == 0) {
    return v.substr(prefix.size());
  }
  return std::string();
}

/* Deadline for control I/O: the callback budget the library reported,
 * clamped to a floor so a tiny remainder still gets a fair socket
 * wait instead of an immediate failure. */
int controlDeadlineMs(const hipObjTransferReqV2_t* req) {
  uint32_t r = req->remainingMs;
  if (r == 0) r = kRdmaTimeoutSecs * 1000;
  return static_cast<int>(r);
}

/* Control-plane authority: the configured control endpoint wins;
 * the S3 object URL is only a fallback. The endpoint is a full
 * "http(s)://host:port" URI, so strip the scheme for dialing and
 * signing; connectTo re-derives the scheme itself for the TLS check.
 * The returned string keeps the scheme prefix (when present) so the
 * caller can reject HTTPS uniformly. */
std::string controlAuthorityUri(V2CallbackCtx* c,
                                const hipObjTransferReqV2_t* req) {
  if (req->endpoint != nullptr &&
      req->endpoint->controlEndpoint != nullptr &&
      req->endpoint->controlEndpoint[0] != '\0') {
    return std::string(req->endpoint->controlEndpoint);
  }
  /* The object URL fallback carries its own scheme; hand back a
   * scheme-qualified URI so the HTTPS rejection in connectTo applies
   * uniformly and the Host split below handles both shapes. */
  return std::string(c->sctx->url.https ? "https" : "http") + "://" +
         c->sctx->url.HostHeaderValue();
}

/* Authority (host[:port]) of a control URI, for the signed Host
 * header. connectTo tolerates a scheme prefix, but the signed Host
 * must never carry one. */
std::string controlAuthorityHost(const std::string& uri) {
  std::string authority = uri;
  const size_t scheme = authority.find("://");
  if (scheme != std::string::npos) {
    authority = authority.substr(scheme + 3);
  }
  const size_t slash = authority.find('/');
  if (slash != std::string::npos) {
    authority = authority.substr(0, slash);
  }
  return authority;
}

int v2SendPrepare(void* ctx, const hipObjTransferReqV2_t* req,
                  hipObjPrepareReplyV2_t* out) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
  extra.Add(kAmzRdmaToken, req->token ? req->token : "");
  extra.Add(kAmzRdmaPsnHdr, hex24Bridge(req->clientPsn));
  extra.Add(kAmzRdmaCookieHdr, hex32Bridge(req->cookie));
  extra.Add(kAmzRdmaOpHdr, req->method ? req->method : "GET");
  extra.Add(kAmzRdmaTargetHdr,
            req->target ? req->target
                        : buildObjectTarget(req->bucket, req->key,
                                            req->query));
  extra.Add(kAmzRdmaSizeHdr, std::to_string(req->size));
  if (req->offset != 0) {
    extra.Add(kAmzRdmaOffsetHdr, std::to_string(req->offset));
  }

  ControlConn conn;
  const int dl = controlDeadlineMs(req);
  /* One deadline for the whole callback: connect, send, and the
   * response read share it, so the steps cannot collectively spend
   * more than the reported budget. */
  const auto deadlineAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(dl);
  const std::string authorityUri = controlAuthorityUri(c, req);
  const std::string authorityHost = controlAuthorityHost(authorityUri);
  if (!conn.connectToUntil(authorityUri, c->clientNic, deadlineAt)) {
    return -1;
  }
  ControlExchange ex;
  if (!buildControlExchange(ex, c->sctx, kControlPathPrepare, c->clientNic,
                            extra, authorityHost) ||
      !conn.sendAllUntil(ex.bytes, deadlineAt)) {
    return -1;
  }
  std::string head, body;
  if (!conn.readResponseUntil(head, body, deadlineAt)) {
    return -1;
  }
  const int status = statusCodeFromHead(head);
  if (status <= 0) {
    return -1;
  }

  std::memset(out, 0, sizeof(*out));
  out->httpStatus = status;
  std::string proto = headerValue(head, kAmzRdmaProtocol);
  out->protocolEcho = proto == kAmzRdmaProtocolV2 ? 1 : 0;
  std::string pstat = headerValue(head, "X-Amz-Rdma-Protocol-Status");
  out->unsupportedMarker = pstat == "unsupported" ? 1 : 0;

  std::string srv_token = replyTokenPayload(headerValue(head, kAmzRdmaReplyHdr));
  if (!srv_token.empty()) {
    std::snprintf(out->serverToken, sizeof(out->serverToken), "%s",
                  srv_token.c_str());
  }
  std::string session = headerValue(head, kAmzRdmaSessionHdr);
  if (!session.empty()) {
    std::snprintf(out->session, sizeof(out->session), "%s", session.c_str());
  }
  /* Strict bounded numeric parsing: full-string hex/decimal only.
   * Leading signs, whitespace, and 0x prefixes are rejected so the
   * bare-hex wire encodings cannot smuggle in other strtoull forms. */
  auto strictUlong = [](const std::string& v, int base,
                        unsigned long long max) -> unsigned long long {
    if (v.empty()) return static_cast<unsigned long long>(-1);
    const bool hex = (base == 16);
    for (char ch : v) {
      const bool ok = hex ? std::isxdigit(static_cast<unsigned char>(ch))
                          : (ch >= '0' && ch <= '9');
      if (!ok) return static_cast<unsigned long long>(-1);
    }
    char* endp = nullptr;
    errno = 0;
    unsigned long long n = std::strtoull(v.c_str(), &endp, base);
    if (errno != 0 || endp == nullptr || *endp != '\0' || n > max) {
      return static_cast<unsigned long long>(-1);
    }
    return n;
  };
  std::string psn = headerValue(head, kAmzRdmaPsnHdr);
  if (!psn.empty()) {
    unsigned long long n = strictUlong(psn, 16, 0xffffffu);
    if (n == static_cast<unsigned long long>(-1)) return -1;
    out->serverPsn = static_cast<uint32_t>(n);
  }
  std::string saddr = headerValue(head, kAmzRdmaMrAddrHdr);
  std::string srkey = headerValue(head, kAmzRdmaMrRkeyHdr);
  if (!saddr.empty() || !srkey.empty()) {
    unsigned long long a = strictUlong(saddr, 16, ~0ULL);
    unsigned long long r = strictUlong(srkey, 16, 0xffffffffu);
    if (a == static_cast<unsigned long long>(-1) ||
        r == static_cast<unsigned long long>(-1)) {
      /* Malformed or partial staging: report it as absent (not as an
       * S3 error) so the core's admission rule classifies the reply
       * as InvalidValue for PUT before anything reaches READY. A GET
       * never needed staging, so it proceeds unaffected. */
      out->stagingPresent = 0;
      out->stagingAddr = 0;
      out->stagingRkey = 0;
      return 0;
    }
    out->stagingAddr = a;
    out->stagingRkey = static_cast<uint32_t>(r);
    out->stagingPresent = (a != 0 && r != 0) ? 1 : 0;
  }
  return 0;
}

int v2SendReadyRequest(void* ctx, const hipObjTransferReqV2_t* req) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);
  if (c->readyPending) {
    return -1; /* one exchange at a time per context */
  }

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
  extra.Add(kAmzRdmaSessionHdr, req->session ? req->session : "");
  extra.Add(kAmzRdmaCookieHdr, hex32Bridge(req->cookie));
  extra.Add(kAmzRdmaQpnHdr, hex64Bridge(req->clientQpn));
  extra.Add(kAmzRdmaMrAddrHdr, hex64Bridge(req->clientMrAddr));
  extra.Add(kAmzRdmaMrRkeyHdr, hex32Bridge(req->clientMrRkey));

  const int dl = controlDeadlineMs(req);
  const auto deadlineAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(dl);
  const std::string authorityUri = controlAuthorityUri(c, req);
  const std::string authorityHost = controlAuthorityHost(authorityUri);
  if (!c->readyConn.connectToUntil(authorityUri, c->clientNic,
                                   deadlineAt)) {
    return -1;
  }
  ControlExchange ex;
  if (!buildControlExchange(ex, c->sctx, kControlPathReady, c->clientNic,
                            extra, authorityHost) ||
      !c->readyConn.sendAllUntil(ex.bytes, deadlineAt)) {
    /* Partial/failed write: the exchange is aborted; the connection
     * is closed, never pooled, and finishReady will not be called. */
    c->readyConn.close();
    return -1;
  }
  c->readyPending = true;
  return 0;
}

int v2FinishReady(void* ctx, const hipObjTransferReqV2_t* req,
                  hipObjFinalReplyV2_t* out) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);
  if (!c->readyPending) {
    return -1;
  }
  std::string head, body;
  if (!c->readyConn.readResponse(head, body, controlDeadlineMs(req))) {
    c->readyConn.close();
    c->readyPending = false;
    return -1;
  }
  /* The exchange reached its terminal disposition: the connection is
   * closed after one complete response (no pooling of v2 sockets). */
  c->readyConn.close();
  c->readyPending = false;

  std::memset(out, 0, sizeof(*out));
  out->httpStatus = statusCodeFromHead(head);
  std::string proto = headerValue(head, kAmzRdmaProtocol);
  out->protocolEcho = proto == kAmzRdmaProtocolV2 ? 1 : 0;

  std::string bytes_hdr = headerValue(head, kAmzRdmaBytesTransferred);
  if (!bytes_hdr.empty()) {
    char* endp = nullptr;
    unsigned long long n = std::strtoull(bytes_hdr.c_str(), &endp, 10);
    if (endp != nullptr && *endp == '\0') {
      out->bytes = n;
    }
  }

  std::string echo = headerValue(head, kAmzRdmaCookieHdr);
  if (!echo.empty()) {
    out->cookieEcho = hexToU32(echo);
    out->cookiePresent = out->cookieEcho != 0 || echo == "00000000" ? 1 : 0;
  }

  std::string etag = headerValue(head, "X-Amz-Rdma-Etag");
  if (!etag.empty()) {
    std::string trimmed = minio::utils::Trim(etag, '"');
    std::snprintf(out->etag, sizeof(out->etag), "%s", trimmed.c_str());
    c->sctx->etag = trimmed;
  }

  std::string csum = headerValue(head, "X-Amz-Rdma-Checksum");
  if (!csum.empty()) {
    /* Wire format is "CRC64NVME <base64>" exactly: the algorithm is
     * part of the contract, and the payload must be the canonical
     * 12-char base64 of 8 bytes (11 data + '='). Anything else - an
     * unknown algorithm or malformed base64 - is rejected rather
     * than stored as if it were a CRC64NVME value. */
    static const char kCsumPrefix[] = "CRC64NVME ";
    const size_t plen = sizeof(kCsumPrefix) - 1;
    bool csumOk = false;
    std::string payload;
    if (csum.compare(0, plen, kCsumPrefix) == 0 &&
        csum.size() == plen + 12) {
      payload = csum.substr(plen);
      csumOk = payload.size() == 12 && payload[11] == '=';
      for (size_t i = 0; csumOk && i < 11; ++i) {
        const char ch = payload[i];
        const bool b64 =
          (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        if (!b64) {
          csumOk = false;
        }
      }
    }
    if (!csumOk) {
      return -1;
    }
    std::snprintf(out->checksumB64, sizeof(out->checksumB64), "%s",
                  payload.c_str());
    c->sctx->checksum = payload;
  }
  return 0;
}

int v2SendCancel(void* ctx, const hipObjTransferReqV2_t* req) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
  extra.Add(kAmzRdmaSessionHdr, req->session ? req->session : "");

  /* The cancel is best-effort with its own fresh budget; a pending
   * READY exchange on this context is aborted (closed, undrained)
   * because the transfer deadline is spent. */
  if (c->readyPending) {
    c->readyConn.close();
    c->readyPending = false;
  }

  ControlConn conn;
  const int dl = controlDeadlineMs(req);
  const auto deadlineAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(dl);
  const std::string authorityUri = controlAuthorityUri(c, req);
  const std::string authorityHost = controlAuthorityHost(authorityUri);
  if (!conn.connectToUntil(authorityUri, c->clientNic, deadlineAt)) {
    return 0; /* best effort */
  }
  ControlExchange ex;
  if (!buildControlExchange(ex, c->sctx, kControlPathCancel, c->clientNic,
                            extra, authorityHost) ||
      !conn.sendAllUntil(ex.bytes, deadlineAt)) {
    return 0;
  }
  std::string head, body;
  if (!conn.readResponseUntil(head, body, deadlineAt)) {
    return 0;
  }
  return 0;
}

// v2 entry points ---------------------------------------------------------

ssize_t rdmaPutV2(S3RdmaContext* sctx, void* buf, size_t size) {
  V2CallbackCtx cbctx{sctx, clientNic()};
  hipObjOpsV2_t ops{};
  ops.sendPrepare = v2SendPrepare;
  ops.sendReadyRequest = v2SendReadyRequest;
  ops.finishReady = v2FinishReady;
  ops.sendCancel = v2SendCancel;

  std::string query;
  if (!sctx->uploadId.empty()) {
    if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
      return -1;
    }
    query = "uploadId=" + sctx->uploadId +
            "&partNumber=" + std::to_string(sctx->partNumber);
  }

  hipObjError_t err = hipObjPutV2(sctx->bucket.c_str(), sctx->object.c_str(),
                                  buf, static_cast<uint64_t>(size), 0,
                                  query.empty() ? nullptr : query.c_str(), &ops,
                                  &cbctx);

  if (err.opError == hipObjNotSupported) {
    return kRdmaNotSupported;
  }
  return (err.opError == hipObjSuccess) ? static_cast<ssize_t>(size) : -1;
}

ssize_t rdmaGetV2(S3RdmaContext* sctx, void* buf, size_t size) {
  V2CallbackCtx cbctx{sctx, clientNic()};
  hipObjOpsV2_t ops{};
  ops.sendPrepare = v2SendPrepare;
  ops.sendReadyRequest = v2SendReadyRequest;
  ops.finishReady = v2FinishReady;
  ops.sendCancel = v2SendCancel;

  hipObjError_t err = hipObjGetV2(sctx->bucket.c_str(), sctx->object.c_str(),
                                  buf, static_cast<uint64_t>(size), 0, nullptr,
                                  &ops, &cbctx);

  if (err.opError == hipObjNotSupported) {
    return kRdmaNotSupported;
  }
  return (err.opError == hipObjSuccess) ? static_cast<ssize_t>(size) : -1;
}

} // namespace

ssize_t rdmaPut(S3RdmaContext* sctx, const char* token, const void* buf,
                size_t size) {
  char rdma_token[512];
  std::snprintf(rdma_token, sizeof(rdma_token), "%s:%016lx:%016lx", token,
                reinterpret_cast<uintptr_t>(buf),
                static_cast<unsigned long>(size));

  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::http::Url url;
  const std::string& region = sctx->region;

  if (!sctx->uploadId.empty()) {
    query_params.Add("uploadId", sctx->uploadId);
    if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
      return -1;
    }
    query_params.Add("partNumber", std::to_string(sctx->partNumber));
  }

  if (minio::error::Error err =
        sctx->url.BuildUrl(url, minio::http::Method::kPut, region, query_params,
                           sctx->bucket, sctx->object)) {
    return -1;
  }

  std::string host = url.HostHeaderValue();

  minio::utils::Multimap sign_headers;
  sign_headers.Add("Host", host);
  sign_headers.Add("x-amz-date", date.ToAmzDate());
  sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
  sign_headers.Add(kAmzRdmaToken, rdma_token);
  sign_headers.Add("Content-Type", "application/octet-stream");
  sign_headers.Add("Content-Length", "0");

  if (!sctx->checksum.empty()) {
    sign_headers.Add("x-amz-checksum-crc64nvme", sctx->checksum);
  }

  if (!creds.session_token.empty()) {
    sign_headers.Add("X-Amz-Security-Token", creds.session_token);
  }

  minio::signer::SignV4S3(minio::http::Method::kPut, url.path, region,
                          sign_headers, query_params, creds.access_key,
                          creds.secret_key, kUnsignedPayload, date);

  url.query_string = query_params.ToQueryString();

  minio::http::Request req(minio::http::Method::kPut, url);
  req.headers = sign_headers;
  req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
  req.timeout_secs = kRdmaTimeoutSecs;

  std::string client_nic = clientNicFromToken(token);
  if (!client_nic.empty()) {
    req.nic_interface = client_nic;
  }

  minio::http::Response res = req.Execute();
  if (!res.error.empty()) {
    return -1;
  }

  std::string etag = res.headers.GetFront("etag");
  if (res.status_code == 200 && !etag.empty()) {
    sctx->etag = minio::utils::Trim(etag, '"');
    return static_cast<ssize_t>(size);
  }

  int reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
  if (reply_code == static_cast<int>(kRdmaNotSupported)) {
    return kRdmaNotSupported;
  }
  if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyNoContent) {
    return -1;
  }

  std::string resp_checksum = res.headers.GetFront("x-amz-checksum-crc64nvme");
  if (!resp_checksum.empty()) {
    sctx->checksum = resp_checksum;
  }

  sctx->etag = minio::utils::Trim(etag, '"');
  return static_cast<ssize_t>(size);
}

ssize_t rdmaGet(S3RdmaContext* sctx, const char* token, const void* buf,
                size_t size) {
  char rdma_token[512];
  std::snprintf(rdma_token, sizeof(rdma_token), "%s:%016lx:%016lx", token,
                reinterpret_cast<uintptr_t>(buf),
                static_cast<unsigned long>(size));

  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::http::Url url;
  const std::string& region = sctx->region;

  if (minio::error::Error err =
        sctx->url.BuildUrl(url, minio::http::Method::kGet, region, query_params,
                           sctx->bucket, sctx->object)) {
    return -1;
  }

  std::string host = url.HostHeaderValue();

  minio::utils::Multimap sign_headers;
  sign_headers.Add("Host", host);
  sign_headers.Add("x-amz-date", date.ToAmzDate());
  sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
  sign_headers.Add(kAmzRdmaToken, rdma_token);

  if (!creds.session_token.empty()) {
    sign_headers.Add("X-Amz-Security-Token", creds.session_token);
  }

  minio::signer::SignV4S3(minio::http::Method::kGet, url.path, region,
                          sign_headers, query_params, creds.access_key,
                          creds.secret_key, kUnsignedPayload, date);

  minio::http::Request req(minio::http::Method::kGet, url);
  req.headers = sign_headers;
  req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
  req.timeout_secs = kRdmaTimeoutSecs;

  std::string client_nic = clientNicFromToken(token);
  if (!client_nic.empty()) {
    req.nic_interface = client_nic;
  }

  minio::http::Response res = req.Execute();
  if (!res.error.empty()) {
    return -1;
  }

  int reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
  if (reply_code == static_cast<int>(kRdmaNotSupported)) {
    return kRdmaNotSupported;
  }
  if (reply_code != kRdmaReplySuccess &&
      reply_code != kRdmaReplyPartialContent) {
    return -1;
  }

  std::string bytes_hdr = res.headers.GetFront(kAmzRdmaBytesTransferred);
  if (!bytes_hdr.empty()) {
    try {
      long long n = std::stoll(bytes_hdr);
      if (n < 0) {
        return -1;
      }
      return static_cast<ssize_t>(n);
    } catch (const std::exception&) {
      return -1;
    }
  }

  return static_cast<ssize_t>(size);
}

ssize_t rdmaPutWithRetry(S3RdmaContext* ctx, void* buf, size_t size) {
  // Try the v2 protocol first; fall back to v1 only when the server
  // explicitly signals it does not support hipobj-rc-v2. Any other
  // failure propagates the terminal sentinel: the buffer and transfer
  // state are uncertain, so an HTTP retry must not touch the buffer.
  ssize_t ret = rdmaPutV2(ctx, buf, size);
  if (ret != kRdmaNotSupported) {
    return ret > 0 ? ret : kRdmaV2Failed;
  }

  ret = -1;
  for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
    char* token = nullptr;
    hipObjError_t terr = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT,
                                            &token);
    if (terr.opError != hipObjSuccess || token == nullptr) {
      return -1;
    }
    ret = rdmaPut(ctx, token, buf, size);
    hipObjPutRdmaToken(token);
    if (ret > 0 || ret == kRdmaNotSupported) {
      return ret;
    }
  }
  return ret;
}

ssize_t rdmaGetWithRetry(S3RdmaContext* ctx, void* buf, size_t size) {
  // Same policy as PUT: only an explicit "unsupported" reply may
  // downgrade the path; any other v2 failure propagates without an
  // HTTP retry (the buffer and transfer state are uncertain).
  ssize_t ret = rdmaGetV2(ctx, buf, size);
  if (ret != kRdmaNotSupported) {
    return ret > 0 ? ret : kRdmaV2Failed;
  }

  ret = -1;
  for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
    char* token = nullptr;
    hipObjError_t terr = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET,
                                            &token);
    if (terr.opError != hipObjSuccess || token == nullptr) {
      return -1;
    }
    ret = rdmaGet(ctx, token, buf, size);
    hipObjPutRdmaToken(token);
    if (ret > 0 || ret == kRdmaNotSupported) {
      return ret;
    }
  }
  return ret;
}

} // namespace hipobj::minio
