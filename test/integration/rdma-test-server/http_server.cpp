/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "http_server.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "hipobj-parse.h"
#include "hipobj-print.h"
#include "hipobj-sockaddr.h"
#include "hipobj-span.h"

namespace hipobj::test {

namespace {

    std::string toLower(std::string s)
    {
        for (char &c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    }

    std::string trim(const std::string &s)
    {
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
            ++start;
        }
        size_t end = s.size();
        while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\r')) {
            --end;
        }
        return s.substr(start, end - start);
    }

    /* The most connections the kernel queues before accept() takes them */
    constexpr int kListenBacklog = 64;

    /* How much each recv() reads */
    constexpr size_t kRecvChunk = size_t{64} * 1024;

    /* Receive caps: headers and declared body length. */
    constexpr size_t kMaxHeaderBytes = size_t{64} * 1024;
    constexpr size_t kMaxBodyBytes   = size_t{1024} * 1024;

    const char *reasonPhrase(int status)
    {
        switch (status) {
            case kHttpOk:
                return "OK";
            case kHttpNoContent:
                return "No Content";
            case kHttpBadRequest:
                return "Bad Request";
            case kHttpForbidden:
                return "Forbidden";
            case kHttpNotFound:
                return "Not Found";
            case kHttpMethodNotAllowed:
                return "Method Not Allowed";
            case kHttpRequestTimeout:
                return "Request Timeout";
            case kHttpConflict:
                return "Conflict";
            case kHttpContentTooLarge:
                return "Content Too Large";
            case kHttpInternalServerError:
                return "Internal Server Error";
            case kHttpNotImplemented:
                return "Not Implemented";
            case kHttpServiceUnavailable:
                return "Service Unavailable";
            default:
                return "Status";
        }
    }

    /* Sets a socket timeout (SO_SNDTIMEO or SO_RCVTIMEO) to the time left
     * before the absolute deadline, capped per syscall so the caller's
     * loop re-checks the deadline. Returns false once it has passed. */
    bool setTimeoutUntil(int fd, int optname, std::chrono::steady_clock::time_point deadline)
    {
        constexpr std::chrono::microseconds kMaxPerCall = std::chrono::seconds(5);

        /* Round up: a zero timeval would mean no timeout at all */
        auto remain =
            std::chrono::ceil<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        if (remain <= std::chrono::microseconds::zero()) {
            return false;
        }
        if (remain > kMaxPerCall) {
            remain = kMaxPerCall;
        }
        /* Durations with timeval's field types, so no casts are needed */
        const auto secs = std::chrono::duration_cast<std::chrono::duration<time_t>>(remain);
        const auto usecs =
            std::chrono::duration_cast<std::chrono::duration<suseconds_t, std::micro>>(remain - secs);
        timeval tv{};
        tv.tv_sec  = secs.count();
        tv.tv_usec = usecs.count();
        setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv));
        return true;
    }

    /* Sends the whole buffer honoring the absolute deadline; MSG_NOSIGNAL
     * keeps a vanished peer from killing the process. Returns false on
     * failure or deadline expiry. */
    bool sendAll(int fd, const std::string &data, std::chrono::steady_clock::time_point deadline)
    {
        size_t sent = 0;
        while (sent < data.size()) {
            if (!setTimeoutUntil(fd, SO_SNDTIMEO, deadline)) {
                return false;
            }
            const std::string_view rest = std::string_view(data).substr(sent);
            const ssize_t          n    = ::send(fd, rest.data(), rest.size(), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

} // namespace

HttpServer::HttpServer(int port) : listen_fd_(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
{
    if (!listen_fd_) {
        return;
    }
    int opt = 1;
    setsockopt(listen_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    if (bind(listen_fd_.get(), hipObj::asSockaddr(addr), sizeof(addr)) != 0) {
        /* Silence here is what made a busy port look like a crash: the object
         * went on being constructed, and the abort arrived later from the
         * accept loop. Say which port and why, once, where it happened. */
        const std::string reason = std::generic_category().message(errno);
        hipObj::printTo(stderr, "hipobj-rdma-test-server: cannot bind port {}: {}\n", port, reason);
        listen_fd_.reset();
        return;
    }
    listen(listen_fd_.get(), kListenBacklog);
}

HttpServer::~HttpServer()
{
    stop();
}

HttpServer::Worker::Worker(HttpServer &server, hipObj::UniqueFd client) noexcept
    : server_(&server), client_(std::move(client))
{
}

HttpServer::Worker::Worker(Worker &&other) noexcept
    : server_(std::exchange(other.server_, nullptr)), client_(std::move(other.client_))
{
}

HttpServer::Worker::~Worker()
{
    if (server_ != nullptr) {
        std::scoped_lock<std::mutex> guard(server_->workersMtx_);
        server_->activeFds_.erase(client_.get());
        server_->liveWorkers_--;
        server_->workerDoneCv_.notify_all();
    }
    /* client_ is closed after this, once it's no longer registered */
}

void
HttpServer::setHandler(HttpHandler handler)
{
    handler_ = std::move(handler);
}

bool
HttpServer::startThreaded()
{
    if (!listen_fd_) {
        return false;
    }
    acceptLoop_ = std::jthread([this](const std::stop_token &stop) { runThreaded(stop); });
    return true;
}

void
HttpServer::runThreaded(const std::stop_token &stop)
{
    /* Accept loop; runs on the tracked thread created by
     * startThreaded() so stop() can join it as the production
     * barrier. */
    if (!listen_fd_) {
        /* When the loop used select(), FD_SET(-1) was a glibc fortify
         * abort, so a failed bind killed the process with "bit out of
         * range 0 - FD_SETSIZE on fd_set" and no mention of a port. poll()
         * just ignores a negative fd, so the loop would wait on nothing.
         * startThreaded() refuses to spawn the thread, but the loop must
         * hold the invariant itself: it is also reachable directly. */
        return;
    }
    while (!stop.stop_requested()) {
        pollfd pfd{};
        pfd.fd          = listen_fd_.get();
        pfd.events      = POLLIN;
        const int ready = poll(&pfd, 1, 200); /* wake every 200 ms for stop checks */
        if (ready <= 0) {
            continue;
        }
        hipObj::UniqueFd client(accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!client) {
            continue;
        }
        /* Linger off is the server-wide invariant: close/shutdown never
         * block and buffered response bytes are delivered gracefully. */
        linger ling{};
        ling.l_onoff  = 0;
        ling.l_linger = 0;
        if (setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &ling, sizeof(ling)) != 0) {
            continue;
        }
        /* Detached workers: each owns its lifetime through its Worker,
         * which unregisters the fd before closing it. stop() waits on the
         * live count (the join barrier above guarantees no late
         * registration follows). */
        std::optional<Worker> worker = registerWorker(std::move(client), stop);
        if (!worker) {
            break;
        }
        try {
            std::thread([this, conn = std::move(*worker)] { handleConnection(conn.fd()); }).detach();
        }
        catch (...) {
            /* Thread creation failed. The Worker the lambda held has
             * already rolled the registration back, so stop()'s
             * live-count wait never blocks on a ghost. */
        }
    }
}

std::optional<HttpServer::Worker>
HttpServer::registerWorker(hipObj::UniqueFd client, const std::stop_token &stop)
{
    std::scoped_lock<std::mutex> guard(workersMtx_);
    if (stop.stop_requested()) {
        return std::nullopt;
    }
    activeFds_.insert(client.get());
    liveWorkers_++;
    return std::optional<Worker>(std::in_place, *this, std::move(client));
}

void
HttpServer::stop()
{
    /* Only the first call does anything, and only once startThreaded()
     * has started the accept loop. Without the loop, there are no workers
     * to stop. */
    if (!acceptLoop_.request_stop()) {
        return;
    }
    if (listen_fd_) {
        shutdown(listen_fd_.get(), SHUT_RDWR);
    }
    /* Wake every blocked worker: a dup'd handle stays valid even if
     * the worker closes the original first, so shutdown cannot hit
     * a recycled fd. */
    {
        std::vector<hipObj::UniqueFd> dups;
        {
            std::scoped_lock<std::mutex> guard(workersMtx_);
            for (const int fd : activeFds_) {
                hipObj::UniqueFd d(fcntl(fd, F_DUPFD_CLOEXEC, 0));
                if (d) {
                    dups.push_back(std::move(d));
                }
                else {
                    shutdown(fd, SHUT_RDWR); /* linger-off: never blocks */
                }
            }
        }
        for (const hipObj::UniqueFd &d : dups) {
            shutdown(d.get(), SHUT_RDWR);
        }
    }
    /* Worker production barrier: joining the accept-loop thread
     * guarantees no registration can follow the live-count wait
     * below. */
    acceptLoop_.join();
    /* All workers are detached: wait for the live count to reach
     * zero. The FD shutdown above unblocks any worker stuck in a
     * socket call, and each worker decrements the count only after
     * removing its fd - so stop() returns with the registry empty
     * and no thread touching server state. */
    {
        std::unique_lock<std::mutex> lock(workersMtx_);
        workerDoneCv_.wait(lock, [this] { return liveWorkers_ == 0; });
    }
}

void
HttpServer::handleConnection(int client)
{
    /* Absolute receive deadline (request bytes only). */
    const auto                   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string                  raw;
    std::array<char, kRecvChunk> buf{};
    size_t                       headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        if (!setTimeoutUntil(client, SO_RCVTIMEO, deadline)) {
            return;
        }
        const ssize_t n = recv(client, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return;
        }
        raw.append(hipObj::asStringView(std::span(buf).first(static_cast<size_t>(n))));
        if (raw.size() > kMaxHeaderBytes) {
            return;
        }
        headerEnd = raw.find("\r\n\r\n");
    }
    /* Body per Content-Length (control requests carry zero). */
    const size_t headerLen  = headerEnd + 4;
    size_t       contentLen = 0;
    {
        HttpRequest head = parseRequest(raw.substr(0, headerLen));
        const auto  it   = head.headers.find("content-length");
        if (it != head.headers.end()) {
            const std::optional<size_t> len = hipObj::parseNumber<size_t>(it->second);
            if (!len || *len > kMaxBodyBytes) {
                return;
            }
            contentLen = *len;
        }
    }
    /* Body receive honors the same absolute deadline as the header:
     * a slow trickle cannot extend it, and a short body is rejected
     * rather than half-delivered. */
    bool bodyComplete = false;
    while (raw.size() - headerLen < contentLen) {
        if (!setTimeoutUntil(client, SO_RCVTIMEO, deadline)) {
            break;
        }
        const ssize_t n = recv(client, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            break;
        }
        raw.append(hipObj::asStringView(std::span(buf).first(static_cast<size_t>(n))));
    }
    bodyComplete = raw.size() - headerLen >= contentLen;
    if (!bodyComplete) {
        return;
    }

    HttpRequest req         = parseRequest(raw.substr(0, headerLen));
    req.body                = raw.substr(headerLen);
    req.contentLength       = contentLen;
    const HttpResponse resp = handler_(req);

    /* Response transmission deadline: 5s from handler completion. A
     * response that can't be sent is dropped with the connection. */
    const auto txDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    try {
        const std::string out = serializeResponse(resp);
        (void)sendAll(client, out, txDeadline);
    }
    catch (...) {
        /* Nothing to do: the connection closes either way */
    }
}

HttpRequest
parseRequest(const std::string &raw)
{
    HttpRequest        req;
    std::istringstream iss(raw);
    std::string        line;
    if (!std::getline(iss, line)) {
        return req;
    }
    {
        std::istringstream ls(line);
        ls >> req.method >> req.path;
    }
    while (std::getline(iss, line) && line != "\r" && !line.empty()) {
        if (line.back() == '\r') {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = toLower(trim(line.substr(0, colon)));
        const std::string val = trim(line.substr(colon + 1));
        req.headers[key]      = val;
    }
    std::ostringstream body;
    body << iss.rdbuf();
    req.body = body.str();
    return req;
}

std::string
serializeResponse(const HttpResponse &resp)
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << reasonPhrase(resp.status) << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    for (const auto &[k, v] : resp.headers) {
        oss << k << ": " << v << "\r\n";
    }
    oss << "\r\n";
    oss << resp.body;
    return oss.str();
}

} // namespace hipobj::test
