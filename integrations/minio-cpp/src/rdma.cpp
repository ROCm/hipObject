/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/rdma.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <hipobj.h>
#include <miniocpp/credentials.h>
#include <miniocpp/error.h>
#include <miniocpp/http.h>
#include <miniocpp/providers.h>
#include <miniocpp/request.h>
#include <miniocpp/signer.h>
#include <miniocpp/utils.h>

#include "bytes-transferred.h"
#include "hipobj-span.h"
#include "hipobj_minio/context.h"

namespace hipobj::minio {

/* The enclosing namespace name shadows the minio-cpp root namespace
 * for qualified lookups; rebind it, and keep the using-directive for
 * unqualified names the rest of this file already uses. */
namespace minio = ::minio;
using namespace ::minio;

namespace {

    // A failed transfer used to surface as a bare -1 with no indication of which
    // of the dozen failure points produced it. Set HIPOBJ_RDMA_DEBUG=1 to get one.
    bool rdmaDebugEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("HIPOBJ_RDMA_DEBUG");
            return v != nullptr && v[0] != '\0' && v[0] != '0';
        }();
        return on;
    }

    // Prints args, streamed one after another, as a line on stderr when
    // HIPOBJ_RDMA_DEBUG is set. The line is built first and written with one
    // fprintf(), which POSIX makes atomic with respect to other stdio calls
    // on the stream, so lines from concurrent transfers don't interleave. One
    // std::cerr insertion has no such guarantee.
    template <typename... Args> void rdmaTrace(const Args &...args)
    {
        if (!rdmaDebugEnabled()) {
            return;
        }
        std::ostringstream line;
        line << "hipobj: rdma: ";
        (line << ... << args);
        line << '\n';
        const std::string text = line.str();
        (void)std::fprintf(stderr, "%s", text.c_str());
    }

    // The HTTP status in an x-amz-rdma-reply value, or std::nullopt if it's
    // malformed. A server that doesn't support RDMA leaves the header out,
    // which gives kRdmaReplyNotImplemented, as an explicit 501 does.
    std::optional<int> parseRdmaReply(const std::string &rdma_reply)
    {
        if (rdma_reply.empty()) {
            return kRdmaReplyNotImplemented;
        }
        int           httpCode = 0;
        hipObjError_t err      = hipObjParseRdmaReply(rdma_reply.c_str(), rdma_reply.size(), &httpCode);
        if (err.opError != hipObjSuccess) {
            return std::nullopt;
        }
        return httpCode;
    }

    std::string clientNicFromToken(const char *token)
    {
        char          nicIp[32];
        hipObjError_t err = hipObjTokenClientNic(token, nicIp, sizeof(nicIp));
        if (err.opError != hipObjSuccess || nicIp[0] == '\0') {
            return {};
        }
        return std::string(nicIp);
    }

    // A response header's value, or std::nullopt if the response doesn't
    // have the header. GetFront() alone returns "" for both.
    std::optional<std::string> headerValue(const minio::utils::Multimap &headers, const char *name)
    {
        if (!headers.Contains(name)) {
            return std::nullopt;
        }
        return headers.GetFront(name);
    }

    // Releases a token from hipObjGetRdmaToken(). Owning the token this way
    // releases it on every path, including when building or sending the
    // request throws.
    struct RdmaTokenDeleter {
        void operator()(char *token) const noexcept
        {
            (void)hipObjPutRdmaToken(token);
        }
    };

    using RdmaTokenPtr = std::unique_ptr<char, RdmaTokenDeleter>;

    // ---------------------------------------------------------------------------
    // v2 callback context: carries the per-transfer minio credentials and
    // endpoint so the three hipObjOpsV2_t callbacks can build signed requests.
    // ---------------------------------------------------------------------------

#ifdef HIPOBJECT_V2_API

    // Copies src into the C string buffer dst, cut to fit
    void copyCString(std::span<char> dst, const std::string &src)
    {
        std::snprintf(dst.data(), dst.size(), "%s", src.c_str());
    }

    std::string hexFixed(uint64_t value, int width)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%0*llx", width, static_cast<unsigned long long>(value));
        return std::string(buf);
    }

    struct ControlConn {
        int         fd = -1;
        std::string host;
        std::string port;

        bool connectTo(const std::string &hostPort, const std::string &nic,
                       std::chrono::steady_clock::time_point deadlineAt);
        void closeConn()
        {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
        bool sendAll(const std::string &bytes, std::chrono::steady_clock::time_point deadlineAt);
        /* One HTTP/1.1 response: status, headers, then Content-Length body. */
        bool readResponse(std::string &head, std::string &body,
                          std::chrono::steady_clock::time_point deadlineAt);
        ~ControlConn()
        {
            closeConn();
        }
    };

    struct V2CallbackCtx {
        S3RdmaContext *sctx;
        std::string    clientNic; // NIC the v2 stack selected (hipObjNicV2)
        /* READY writes the request and parks this socket until finishReady. */
        ControlConn readyConn;
        bool        readyPending = false;
    };

    std::string clientNic()
    {
        char       *nic = hipObjNicV2();
        std::string out = nic != nullptr ? std::string(nic) : std::string();
        hipObjFreeNicV2(nic);
        return out;
    }

    bool ControlConn::connectTo(const std::string &hostPort, const std::string &nic,
                                std::chrono::steady_clock::time_point deadlineAt)
    {
        closeConn();
        std::string host      = hostPort;
        std::string port      = "80";
        bool        https     = false;
        const size_t scheme   = hostPort.find("://");
        if (scheme != std::string::npos) {
            https = hostPort.compare(0, scheme, "https") == 0;
            host  = hostPort.substr(scheme + 3);
        }
        const size_t slash = host.find('/');
        if (slash != std::string::npos) {
            host = host.substr(0, slash);
        }
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos) {
            port = host.substr(colon + 1);
            host = host.substr(0, colon);
        }
        if (https) {
            /* The direct control path is plain HTTP. TLS is out of scope. */
            return false;
        }

        struct addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *list = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0 || list == nullptr) {
            return false;
        }
        int fd = -1;
        for (struct addrinfo *ai = list; ai != nullptr; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) {
                continue;
            }
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (!nic.empty()) {
                struct ifreq ifr{};
                std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", nic.c_str());
                if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0) {
                    struct ifaddrs *ifs = nullptr;
                    if (getifaddrs(&ifs) == 0) {
                        for (struct ifaddrs *i = ifs; i != nullptr; i = i->ifa_next) {
                            if (std::strcmp(i->ifa_name, nic.c_str()) != 0 || i->ifa_addr == nullptr ||
                                i->ifa_addr->sa_family != AF_INET) {
                                continue;
                            }
                            auto *sa = reinterpret_cast<sockaddr_in *>(i->ifa_addr);
                            if (bind(fd, reinterpret_cast<sockaddr *>(sa), sizeof(*sa)) == 0) {
                                break;
                            }
                        }
                        freeifaddrs(ifs);
                    }
                }
            }
            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc == 0) {
                break;
            }
            if (errno == EINPROGRESS) {
                struct pollfd pfd{fd, POLLOUT, 0};
                int           pr   = 0;
                int           left = 0;
                do {
                    left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                deadlineAt - std::chrono::steady_clock::now())
                                                .count());
                    if (left <= 0) {
                        pr = 0;
                        break;
                    }
                    pr = poll(&pfd, 1, left);
                } while (pr < 0 && errno == EINTR);
                if (pr <= 0) {
                    ::close(fd);
                    fd = -1;
                    continue;
                }
                int       err  = 0;
                socklen_t elen = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err == 0) {
                    break;
                }
            }
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(list);
        if (fd < 0) {
            return false;
        }
        /* Stay nonblocking. sendAll and readResponse poll, and treat
         * EAGAIN as "wait again" rather than a hard error. */
        this->fd   = fd;
        this->host = host;
        this->port = port;
        return true;
    }

    bool ControlConn::sendAll(const std::string &bytes, std::chrono::steady_clock::time_point deadlineAt)
    {
        if (fd < 0) {
            return false;
        }
        size_t     off        = 0;
        while (off < bytes.size()) {
            struct pollfd pfd{fd, POLLOUT, 0};
            int           pr   = 0;
            int           left = 0;
            do {
                left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadlineAt - std::chrono::steady_clock::now())
                                            .count());
                if (left <= 0) {
                    return false;
                }
                pr = poll(&pfd, 1, left);
            } while (pr < 0 && errno == EINTR);
            if (pr <= 0) {
                return false;
            }
            const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool ControlConn::readResponse(std::string &head, std::string &body,
                                   std::chrono::steady_clock::time_point deadlineAt)
    {
        if (fd < 0) {
            return false;
        }
        std::string buf;
        size_t      headEnd    = std::string::npos;
        size_t      contentLen = std::string::npos;
        while (true) {
            if (headEnd == std::string::npos) {
                headEnd = buf.find("\r\n\r\n");
                if (headEnd != std::string::npos) {
                    head = buf.substr(0, headEnd);
                    buf  = buf.substr(headEnd + 4);
                    int    lengthCount = 0;
                    size_t pos         = 0;
                    while (pos < head.size()) {
                        size_t eol = head.find("\r\n", pos);
                        if (eol == std::string::npos) {
                            eol = head.size();
                        }
                        const std::string line = head.substr(pos, eol - pos);
                        pos                    = (eol == head.size()) ? head.size() : eol + 2;
                        const std::string cl = "content-length:";
                        const std::string te = "transfer-encoding:";
                        if (line.size() >= te.size() && strncasecmp(line.c_str(), te.c_str(), te.size()) == 0) {
                            return false;
                        }
                        if (line.size() >= cl.size() && strncasecmp(line.c_str(), cl.c_str(), cl.size()) == 0) {
                            ++lengthCount;
                            const char *val = line.c_str() + cl.size();
                            while (*val == ' ' || *val == '\t') {
                                ++val;
                            }
                            const std::string digits(val);
                            if (digits.empty() || digits.size() > 20 ||
                                digits.find_first_not_of("0123456789") != std::string::npos) {
                                return false;
                            }
                            errno = 0;
                            char                    *endp = nullptr;
                            const unsigned long long n    = std::strtoull(digits.c_str(), &endp, 10);
                            if (errno != 0 || endp == nullptr || *endp != '\0') {
                                return false;
                            }
                            contentLen = static_cast<size_t>(n);
                        }
                    }
                    if (lengthCount != 1) {
                        return false;
                    }
                }
            }
            if (headEnd != std::string::npos && contentLen != std::string::npos && buf.size() >= contentLen) {
                body = buf.substr(0, contentLen);
                return true;
            }
            struct pollfd pfd{fd, POLLIN, 0};
            int           pr   = 0;
            int           left = 0;
            do {
                left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadlineAt - std::chrono::steady_clock::now())
                                            .count());
                if (left <= 0) {
                    return false;
                }
                pr = poll(&pfd, 1, left);
            } while (pr < 0 && errno == EINTR);
            if (pr <= 0) {
                return false;
            }
            char          chunk[4096];
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            buf.append(chunk, static_cast<size_t>(n));
            if (buf.size() > (1u << 20)) {
                return false;
            }
        }
    }

    struct ControlExchange {
        std::string bytes;
    };

    bool buildControlExchange(ControlExchange &out, S3RdmaContext *sctx, const std::string &controlPath,
                              minio::utils::Multimap &extraHeaders, const std::string &hostHeaderValue)
    {
        minio::utils::UtcTime     date  = minio::utils::UtcTime::Now();
        minio::creds::Credentials creds = sctx->provider->Fetch();
        minio::utils::Multimap    queryParams;
        minio::utils::Multimap    signHeaders;
        signHeaders.Add("Host", hostHeaderValue);
        signHeaders.Add("x-amz-date", date.ToAmzDate());
        signHeaders.Add("x-amz-content-sha256", kUnsignedPayload);
        signHeaders.Add("Content-Length", "0");
        signHeaders.AddAll(extraHeaders);
        if (!creds.session_token.empty()) {
            signHeaders.Add("X-Amz-Security-Token", creds.session_token);
        }
        minio::signer::SignV4S3(minio::http::Method::kPost, controlPath, sctx->region, signHeaders, queryParams,
                                creds.access_key, creds.secret_key, kUnsignedPayload, date);
        std::string req = "POST ";
        req += controlPath;
        req += " HTTP/1.1\r\n";
        for (const std::string &h : signHeaders.ToHttpHeaders()) {
            req += h;
            req += "\r\n";
        }
        req += "Connection: keep-alive\r\n\r\n";
        out.bytes = req;
        return true;
    }

    std::string rawHeaderValue(const std::string &head, const std::string &name)
    {
        std::string lower;
        lower.reserve(name.size());
        for (char c : name) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        size_t pos = 0;
        while (pos < head.size()) {
            size_t eol = head.find("\r\n", pos);
            if (eol == std::string::npos) {
                eol = head.size();
            }
            const std::string line = head.substr(pos, eol - pos);
            pos                    = (eol == head.size()) ? head.size() : eol + 2;
            if (line.size() > lower.size() + 1 && strncasecmp(line.c_str(), lower.c_str(), lower.size()) == 0 &&
                line[lower.size()] == ':') {
                size_t vs = lower.size() + 1;
                while (vs < line.size() && line[vs] == ' ') {
                    ++vs;
                }
                return line.substr(vs);
            }
        }
        return {};
    }

    int statusCodeFromHead(const std::string &head)
    {
        const size_t sp = head.find(' ');
        if (sp == std::string::npos) {
            return 0;
        }
        return static_cast<int>(std::strtol(head.c_str() + sp + 1, nullptr, 10));
    }

    std::string controlDialTarget(const hipObjTransferReqV2_t *req, S3RdmaContext *sctx)
    {
        if (req->endpoint != nullptr && req->endpoint->controlEndpoint != nullptr &&
            req->endpoint->controlEndpoint[0] != '\0') {
            return req->endpoint->controlEndpoint;
        }
        const minio::s3::BaseUrl &base = sctx->url;
        const std::string authority =
            base.port != 0 ? base.host + ":" + std::to_string(base.port) : base.host;
        return std::string(base.https ? "https" : "http") + "://" + authority;
    }

    std::string hostHeaderFromConn(const ControlConn &conn)
    {
        if (conn.port.empty() || conn.port == "80") {
            return conn.host;
        }
        return conn.host + ":" + conn.port;
    }

    int controlDeadlineMs(const hipObjTransferReqV2_t *req)
    {
        uint32_t r = req->remainingMs;
        if (r == 0) {
            r = static_cast<uint32_t>(kRdmaTimeoutSecs * 1000);
        }
        return static_cast<int>(r);
    }

    std::chrono::steady_clock::time_point controlDeadlineAt(const hipObjTransferReqV2_t *req)
    {
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(controlDeadlineMs(req));
    }

    std::string replyTokenFromHeader(const std::string &value)
    {
        const std::string prefix = "200:";
        if (value.size() > prefix.size() && value.compare(0, prefix.size(), prefix) == 0) {
            return value.substr(prefix.size());
        }
        return {};
    }

    int v2SendPrepare(void *ctx, const hipObjTransferReqV2_t *req, hipObjPrepareReplyV2_t *out)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);

        minio::utils::Multimap extra;
        extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
        extra.Add(kAmzRdmaToken, req->token ? req->token : "");
        extra.Add(kAmzRdmaPsn, hexFixed(req->clientPsn, 6));
        extra.Add(kAmzRdmaCookie, hexFixed(req->cookie, 8));
        extra.Add(kAmzRdmaOp, req->method ? req->method : "GET");
        extra.Add(kAmzRdmaTarget, req->target ? req->target : "");
        extra.Add(kAmzRdmaSize, std::to_string(req->size));
        if (req->offset != 0) {
            extra.Add(kAmzRdmaOffset, std::to_string(req->offset));
        }

        ControlConn conn;
        const auto  deadlineAt = controlDeadlineAt(req);
        if (!conn.connectTo(controlDialTarget(req, c->sctx), c->clientNic, deadlineAt)) {
            return -1;
        }
        ControlExchange ex;
        const std::string hostHeader = hostHeaderFromConn(conn);
        if (!buildControlExchange(ex, c->sctx, kControlPathPrepare, extra, hostHeader) ||
            !conn.sendAll(ex.bytes, deadlineAt)) {
            return -1;
        }
        std::string head, body;
        if (!conn.readResponse(head, body, deadlineAt)) {
            return -1;
        }
        const int status = statusCodeFromHead(head);
        if (status <= 0) {
            return -1;
        }

        std::memset(out, 0, sizeof(*out));
        out->httpStatus = status;
        const std::string proto = rawHeaderValue(head, kAmzRdmaProtocol);
        out->protocolEcho       = proto == kAmzRdmaProtocolV2 ? 1 : 0;
        const std::string pstat = rawHeaderValue(head, "X-Amz-Rdma-Protocol-Status");
        out->unsupportedMarker  = pstat == "unsupported" ? 1 : 0;

        const std::string srvToken = replyTokenFromHeader(rawHeaderValue(head, kAmzRdmaReply));
        if (!srvToken.empty()) {
            copyCString(out->serverToken, srvToken);
        }
        const std::string session = rawHeaderValue(head, kAmzRdmaSession);
        if (!session.empty()) {
            copyCString(out->session, session);
        }

        auto strictUlong = [](const std::string &v, int base, unsigned long long max) -> unsigned long long {
            if (v.empty()) {
                return static_cast<unsigned long long>(-1);
            }
            char *endp = nullptr;
            errno      = 0;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, base);
            if (errno != 0 || endp == nullptr || *endp != '\0' || n > max) {
                return static_cast<unsigned long long>(-1);
            }
            return n;
        };
        const std::string psn = rawHeaderValue(head, kAmzRdmaPsn);
        if (!psn.empty()) {
            const unsigned long long n = strictUlong(psn, 16, 0xffffffu);
            if (n == static_cast<unsigned long long>(-1)) {
                return -1;
            }
            out->serverPsn = static_cast<uint32_t>(n);
        }
        const std::string saddr = rawHeaderValue(head, kAmzRdmaMrAddr);
        const std::string srkey = rawHeaderValue(head, kAmzRdmaMrRkey);
        if (!saddr.empty() || !srkey.empty()) {
            const unsigned long long a = strictUlong(saddr, 16, ~0ULL);
            const unsigned long long r = strictUlong(srkey, 16, 0xffffffffu);
            if (a == static_cast<unsigned long long>(-1) || r == static_cast<unsigned long long>(-1)) {
                /* Malformed staging is reported as absent. The core
                 * classifies a PUT without staging as InvalidValue
                 * before READY. A GET does not need staging. */
                out->stagingPresent = 0;
                out->stagingAddr    = 0;
                out->stagingRkey    = 0;
                return 0;
            }
            out->stagingAddr    = a;
            out->stagingRkey    = static_cast<uint32_t>(r);
            out->stagingPresent = (a != 0 && r != 0) ? 1 : 0;
        }
        return 0;
    }

    int v2SendReadyRequest(void *ctx, const hipObjTransferReqV2_t *req)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);
        if (c->readyPending) {
            return -1;
        }

        minio::utils::Multimap extra;
        extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
        extra.Add(kAmzRdmaSession, req->session ? req->session : "");
        extra.Add(kAmzRdmaCookie, hexFixed(req->cookie, 8));
        extra.Add(kAmzRdmaQpn, hexFixed(req->clientQpn, 8));
        extra.Add(kAmzRdmaMrAddr, hexFixed(req->clientMrAddr, 16));
        extra.Add(kAmzRdmaMrRkey, hexFixed(req->clientMrRkey, 8));

        const auto deadlineAt = controlDeadlineAt(req);
        if (!c->readyConn.connectTo(controlDialTarget(req, c->sctx), c->clientNic, deadlineAt)) {
            return -1;
        }
        ControlExchange ex;
        if (!buildControlExchange(ex, c->sctx, kControlPathReady, extra, hostHeaderFromConn(c->readyConn)) ||
            !c->readyConn.sendAll(ex.bytes, deadlineAt)) {
            c->readyConn.closeConn();
            return -1;
        }
        c->readyPending = true;
        return 0;
    }

    int v2FinishReady(void *ctx, const hipObjTransferReqV2_t *req, hipObjFinalReplyV2_t *out)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);
        if (!c->readyPending) {
            return -1;
        }
        std::string head, body;
        if (!c->readyConn.readResponse(head, body, controlDeadlineAt(req))) {
            c->readyConn.closeConn();
            c->readyPending = false;
            return -1;
        }
        c->readyConn.closeConn();
        c->readyPending = false;

        std::memset(out, 0, sizeof(*out));
        out->httpStatus             = statusCodeFromHead(head);
        const std::string proto     = rawHeaderValue(head, kAmzRdmaProtocol);
        out->protocolEcho           = proto == kAmzRdmaProtocolV2 ? 1 : 0;

        const std::string bytesHdr = rawHeaderValue(head, kAmzRdmaBytesTransferred);
        if (!bytesHdr.empty()) {
            char                    *endp = nullptr;
            const unsigned long long n    = std::strtoull(bytesHdr.c_str(), &endp, 10);
            if (endp != nullptr && *endp == '\0') {
                out->bytes = n;
            }
        }

        const std::string echo = rawHeaderValue(head, kAmzRdmaCookie);
        if (!echo.empty()) {
            char                    *endp = nullptr;
            const unsigned long      n    = std::strtoul(echo.c_str(), &endp, 16);
            if (endp != nullptr && *endp == '\0' && n <= 0xffffffffUL) {
                out->cookieEcho    = static_cast<uint32_t>(n);
                out->cookiePresent = 1;
            }
        }

        const std::string etag = rawHeaderValue(head, "X-Amz-Rdma-Etag");
        if (!etag.empty()) {
            const std::string trimmed = minio::utils::Trim(etag, '"');
            copyCString(out->etag, trimmed);
            c->sctx->etag = trimmed;
        }

        const std::string csum = rawHeaderValue(head, "X-Amz-Rdma-Checksum");
        if (!csum.empty()) {
            /* Wire format is "CRC64NVME <base64>" exactly: 11 data
             * characters and a trailing '='. Anything else is rejected
             * rather than stored as a checksum. */
            static const char kCsumPrefix[] = "CRC64NVME ";
            const size_t      plen          = sizeof(kCsumPrefix) - 1;
            bool              csumOk        = false;
            std::string       payload;
            if (csum.compare(0, plen, kCsumPrefix) == 0 && csum.size() == plen + 12) {
                payload = csum.substr(plen);
                csumOk  = payload.size() == 12 && payload[11] == '=';
                for (size_t i = 0; csumOk && i < 11; ++i) {
                    const char ch  = payload[i];
                    const bool b64 = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                     (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
                    if (!b64) {
                        csumOk = false;
                    }
                }
            }
            if (!csumOk || payload.size() >= sizeof(out->checksumB64)) {
                return -1;
            }
            copyCString(out->checksumB64, payload);
            c->sctx->checksum = payload;
        }
        return 0;
    }

    int v2SendCancel(void *ctx, const hipObjTransferReqV2_t *req)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);

        minio::utils::Multimap extra;
        extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
        extra.Add(kAmzRdmaSession, req->session ? req->session : "");
        extra.Add(kAmzRdmaCancel, "1");

        if (c->readyPending) {
            c->readyConn.closeConn();
            c->readyPending = false;
        }

        ControlConn conn;
        const auto  deadlineAt = controlDeadlineAt(req);
        if (!conn.connectTo(controlDialTarget(req, c->sctx), c->clientNic, deadlineAt)) {
            return 0;
        }
        ControlExchange ex;
        if (!buildControlExchange(ex, c->sctx, kControlPathCancel, extra, hostHeaderFromConn(conn)) ||
            !conn.sendAll(ex.bytes, deadlineAt)) {
            return 0;
        }
        std::string head, body;
        (void)conn.readResponse(head, body, deadlineAt);
        return 0;
    }

    // v2 entry points ---------------------------------------------------------

    RdmaResult rdmaPutV2(S3RdmaContext *sctx, void *buf, size_t size)
    {
        V2CallbackCtx cbctx{sctx, clientNic(), {}, false};
        hipObjOpsV2_t ops{};
        ops.sendPrepare       = v2SendPrepare;
        ops.sendReadyRequest  = v2SendReadyRequest;
        ops.finishReady       = v2FinishReady;
        ops.sendCancel        = v2SendCancel;

        std::string query;
        if (!sctx->uploadId.empty()) {
            if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
                rdmaTrace("v2 put: part number ", sctx->partNumber, " out of range");
                return {RdmaStatus::Failed};
            }
            // hipObjPutV2() takes a canonical query: percent-encoded, with
            // the keys sorted. Upload IDs can contain reserved characters
            // such as '+' and '/', and Multimap encodes and sorts.
            minio::utils::Multimap query_params;
            query_params.Add("uploadId", sctx->uploadId);
            query_params.Add("partNumber", std::to_string(sctx->partNumber));
            query = query_params.ToQueryString();
        }

        hipObjError_t err = hipObjPutV2(sctx->bucket.c_str(), sctx->object.c_str(), buf, size, 0,
                                        query.empty() ? nullptr : query.c_str(), &ops, &cbctx);

        if (err.opError == hipObjNotSupported) {
            rdmaTrace("v2 put: unsupported (client v2 is a stub); falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError == hipObjSizeTooLarge) {
            // v2 caps a transfer at 2^31-1 bytes; v1 can move it
            rdmaTrace("v2 put: ", size, " bytes is too large for v2; falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError != hipObjSuccess) {
            rdmaTrace("v2 put: hipObjPutV2: ", hipObjGetErrorString(err.opError));
            return {RdmaStatus::V2Failed};
        }
        return {RdmaStatus::Ok, size};
    }

    RdmaResult rdmaGetV2(S3RdmaContext *sctx, void *buf, size_t size)
    {
        V2CallbackCtx cbctx{sctx, clientNic(), {}, false};
        hipObjOpsV2_t ops{};
        ops.sendPrepare       = v2SendPrepare;
        ops.sendReadyRequest  = v2SendReadyRequest;
        ops.finishReady       = v2FinishReady;
        ops.sendCancel        = v2SendCancel;

        hipObjError_t err =
            hipObjGetV2(sctx->bucket.c_str(), sctx->object.c_str(), buf, size, 0, nullptr, &ops, &cbctx);

        if (err.opError == hipObjNotSupported) {
            rdmaTrace("v2 get: unsupported (client v2 is a stub); falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError == hipObjSizeTooLarge) {
            // v2 caps a transfer at 2^31-1 bytes; v1 can move it
            rdmaTrace("v2 get: ", size, " bytes is too large for v2; falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError != hipObjSuccess) {
            rdmaTrace("v2 get: hipObjGetV2: ", hipObjGetErrorString(err.opError));
            return {RdmaStatus::V2Failed};
        }
        return {RdmaStatus::Ok, size};
    }

#endif /* HIPOBJECT_V2_API */

} // namespace

RdmaResult
rdmaPut(S3RdmaContext *sctx, const char *token, size_t size)
{
    // The header carries one fixed-width token and nothing else; a
    // ":<addr>:<len>" suffix would override the address the token already
    // advertises, and on the staging path the caller's pointer is not that
    // address. See docs/interop.rst.
    const std::string rdma_token(token ? token : "");

    minio::utils::UtcTime     date  = minio::utils::UtcTime::Now();
    minio::creds::Credentials creds = sctx->provider->Fetch();
    minio::utils::Multimap    query_params;
    minio::http::Url          url;
    const std::string        &region = sctx->region;

    if (!sctx->uploadId.empty()) {
        query_params.Add("uploadId", sctx->uploadId);
        if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
            return {RdmaStatus::Failed};
        }
        query_params.Add("partNumber", std::to_string(sctx->partNumber));
    }

    if (minio::error::Error err = sctx->url.BuildUrl(url, minio::http::Method::kPut, region, query_params,
                                                     sctx->bucket, sctx->object)) {
        return {RdmaStatus::Failed};
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

    minio::signer::SignV4S3(minio::http::Method::kPut, url.path, region, sign_headers, query_params,
                            creds.access_key, creds.secret_key, kUnsignedPayload, date);

    url.query_string = query_params.ToQueryString();

    minio::http::Request req(minio::http::Method::kPut, url);
    req.headers              = std::move(sign_headers);
    req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
    req.timeout_secs         = kRdmaTimeoutSecs;

    std::string client_nic = clientNicFromToken(token);
    if (!client_nic.empty()) {
        req.nic_interface = client_nic;
    }

    minio::http::Response res = req.Execute();
    if (!res.error.empty()) {
        rdmaTrace("v1 put: http failed (status=", res.status_code, " error='", res.error, "')");
        return {RdmaStatus::Failed};
    }

    std::string etag = res.headers.GetFront("etag");
    if (res.status_code == 200 && !etag.empty()) {
        sctx->etag = minio::utils::Trim(etag, '"');
        return {RdmaStatus::Ok, size};
    }

    const auto reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
    if (reply_code == kRdmaReplyNotImplemented) {
        return {RdmaStatus::NotSupported};
    }
    if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyNoContent) {
        rdmaTrace("v1 put: rdma reply ", reply_code.value_or(0), " (http status ", res.status_code, ")");
        return {RdmaStatus::Failed};
    }

    std::string resp_checksum = res.headers.GetFront("x-amz-checksum-crc64nvme");
    if (!resp_checksum.empty()) {
        sctx->checksum = resp_checksum;
    }

    sctx->etag = minio::utils::Trim(etag, '"');
    return {RdmaStatus::Ok, size};
}

RdmaResult
rdmaGet(S3RdmaContext *sctx, const char *token, size_t size)
{
    // The header carries one fixed-width token and nothing else; a
    // ":<addr>:<len>" suffix would override the address the token already
    // advertises, and on the staging path the caller's pointer is not that
    // address. See docs/interop.rst.
    const std::string rdma_token(token ? token : "");

    minio::utils::UtcTime     date  = minio::utils::UtcTime::Now();
    minio::creds::Credentials creds = sctx->provider->Fetch();
    minio::utils::Multimap    query_params;
    minio::http::Url          url;
    const std::string        &region = sctx->region;

    if (minio::error::Error err = sctx->url.BuildUrl(url, minio::http::Method::kGet, region, query_params,
                                                     sctx->bucket, sctx->object)) {
        return {RdmaStatus::Failed};
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

    minio::signer::SignV4S3(minio::http::Method::kGet, url.path, region, sign_headers, query_params,
                            creds.access_key, creds.secret_key, kUnsignedPayload, date);

    minio::http::Request req(minio::http::Method::kGet, url);
    req.headers              = std::move(sign_headers);
    req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
    req.timeout_secs         = kRdmaTimeoutSecs;

    std::string client_nic = clientNicFromToken(token);
    if (!client_nic.empty()) {
        req.nic_interface = client_nic;
    }

    minio::http::Response res = req.Execute();
    if (!res.error.empty()) {
        rdmaTrace("v1 get: http failed (status=", res.status_code, " error='", res.error, "')");
        return {RdmaStatus::Failed};
    }

    const auto reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
    if (reply_code == kRdmaReplyNotImplemented) {
        return {RdmaStatus::NotSupported};
    }
    if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyPartialContent) {
        rdmaTrace("v1 get: rdma reply ", reply_code.value_or(0), " (http status ", res.status_code, ")");
        return {RdmaStatus::Failed};
    }

    // Without the header, the whole buffer was transferred. The server can't
    // have transferred more than that.
    const auto bytes = parseBytesTransferred(headerValue(res.headers, kAmzRdmaBytesTransferred), size);
    if (!bytes || *bytes > size) {
        rdmaTrace("v1 get: malformed or out-of-range ", kAmzRdmaBytesTransferred, " header");
        return {RdmaStatus::Failed};
    }
    return {RdmaStatus::Ok, static_cast<size_t>(*bytes)};
}

RdmaResult
rdmaPutWithRetry(S3RdmaContext *ctx, void *buf, size_t size)
{
#ifdef HIPOBJECT_V2_API
    // Try the v2 protocol first; fall back to v1 only when the server
    // explicitly signals it does not support hipobj-rc-v2.
    const RdmaResult v2 = rdmaPutV2(ctx, buf, size);
    if (v2.status != RdmaStatus::NotSupported) {
        return v2;
    }
#endif /* HIPOBJECT_V2_API */
    RdmaResult ret{RdmaStatus::Failed};
    for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("v1 put: token: ", hipObjGetErrorString(terr.opError));
            return {RdmaStatus::Failed};
        }
        // A buffer the NIC cannot reach directly is registered as a host
        // staging buffer, so the bytes have to be there before the server
        // reads it.
        hipObjError_t serr = hipObjBufSync(buf, size, 0, HIPOBJ_SYNC_TO_HOST);
        if (serr.opError != hipObjSuccess) {
            rdmaTrace("v1 put: stage to host: ", hipObjGetErrorString(serr.opError));
            return {RdmaStatus::Failed};
        }
        ret = rdmaPut(ctx, token.get(), size);
        if ((ret.status == RdmaStatus::Ok && ret.bytes > 0) || ret.status == RdmaStatus::NotSupported) {
            return ret;
        }
    }
    return ret;
}

RdmaResult
rdmaGetWithRetry(S3RdmaContext *ctx, void *buf, size_t size)
{
#ifdef HIPOBJECT_V2_API
    const RdmaResult v2 = rdmaGetV2(ctx, buf, size);
    if (v2.status != RdmaStatus::NotSupported) {
        return v2;
    }
#endif /* HIPOBJECT_V2_API */
    RdmaResult ret{RdmaStatus::Failed};
    for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("v1 get: token: ", hipObjGetErrorString(terr.opError));
            return {RdmaStatus::Failed};
        }
        ret = rdmaGet(ctx, token.get(), size);
        if (ret.status == RdmaStatus::Ok && ret.bytes > 0) {
            hipObjError_t serr = hipObjBufSync(buf, size, 0, HIPOBJ_SYNC_TO_DEVICE);
            if (serr.opError != hipObjSuccess) {
                rdmaTrace("v1 get: stage to device: ", hipObjGetErrorString(serr.opError));
                return {RdmaStatus::Failed};
            }
            return ret;
        }
        if (ret.status == RdmaStatus::NotSupported) {
            return ret;
        }
    }
    return ret;
}

} // namespace hipobj::minio
