/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "http_server.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hipobj::test {

namespace {

std::string toLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string trim(const std::string& s) {
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

/* Receive caps: headers and declared body length. */
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;

const char* reasonPhrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 408:
      return "Request Timeout";
    case 409:
      return "Conflict";
    case 413:
      return "Content Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 503:
      return "Service Unavailable";
    default:
      return "Status";
  }
}

/* Sends the whole buffer honoring the absolute deadline; MSG_NOSIGNAL
 * keeps a vanished peer from killing the process. Returns false on
 * failure or deadline expiry. */
bool sendAll(int fd, const std::string& data, uint64_t deadlineMs) {
  size_t sent = 0;
  while (sent < data.size()) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t nowMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    if (nowMs >= deadlineMs) {
      return false;
    }
    uint64_t remain = deadlineMs - nowMs;
    if (remain > 5000) {
      remain = 5000; /* per-syscall cap; the loop re-checks */
    }
    timeval tv{};
    tv.tv_sec = static_cast<long>(remain / 1000);
    tv.tv_usec = static_cast<long>((remain % 1000) * 1000);
    if (remain == 0) {
      tv.tv_usec = 1000; /* at least 1ms */
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
                       MSG_NOSIGNAL);
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

HttpServer::HttpServer(int port) {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return;
  }
  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }
  listen(listen_fd_, 64);
}

HttpServer::~HttpServer() {
  stop();
}

void HttpServer::setHandler(HttpHandler handler) {
  handler_ = std::move(handler);
}

int HttpServer::runOnce(int timeoutMs) {
  if (listen_fd_ < 0 || !handler_) {
    return -1;
  }
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(listen_fd_, &fds);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int ready = select(listen_fd_ + 1, &fds, nullptr, nullptr, &tv);
  if (ready <= 0) {
    return ready;
  }
  int client = accept(listen_fd_, nullptr, nullptr);
  if (client < 0) {
    return -1;
  }
  char buf[65536];
  ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
  if (n <= 0) {
    close(client);
    return 0;
  }
  buf[n] = '\0';
  HttpRequest req = parseRequest(buf);
  HttpResponse resp = handler_(req);
  std::string out = serializeResponse(resp);
  send(client, out.data(), out.size(), MSG_NOSIGNAL);
  close(client);
  return 1;
}

void HttpServer::startThreaded() {
  acceptLoop_ = std::thread([this] {
    runThreaded();
  });
}

void HttpServer::runThreaded() {
  /* Runs the accept loop on the caller's thread. Call via
   * startThreaded() to get a joinable handle stop() can use as
   * the production barrier; calling directly means the caller
   * must not return before calling stop(). */
  while (!stopping_.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listen_fd_, &fds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000; /* wake for stop checks */
    int ready = select(listen_fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ready <= 0) {
      continue;
    }
    int client = accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    /* Linger off is the server-wide invariant: close/shutdown never
     * block and buffered response bytes are delivered gracefully. */
    linger ling{};
    ling.l_onoff = 0;
    ling.l_linger = 0;
    if (setsockopt(client, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling)) != 0) {
      close(client);
      continue;
    }
    /* Shared completion flag: the accept loop joins finished
     * workers without blocking on running ones. Joins happen
     * OUTSIDE the lock, so no path can hold workersMtx_ while
     * waiting on a thread that needs it. */
    /* Detach-based workers: each worker owns its lifetime and
     * signals completion through a shared flag. No thread handles
     * are stored, so there is no joinable-object destruction
     * window and stop() only needs to wait for the flags. The fd
     * is registered before the thread starts (worker actions
     * follow registration) and removed before close (no number
     * recycling window). */
    auto done = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> guard(workersMtx_);
      activeFds_.insert(client);
      liveWorkers_++;
    }
    std::thread([this, client, done] {
      handleConnection(client, /*closeFd=*/false);
      {
        std::lock_guard<std::mutex> guard(workersMtx_);
        activeFds_.erase(client);
        liveWorkers_--;
        workerDoneCv_.notify_all();
      }
      ::close(client);
    }).detach();
    (void)done;
  }
}

void HttpServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
  }
  /* Wake every blocked worker: a dup'd handle stays valid even if
   * the worker closes the original first, so shutdown cannot hit
   * a recycled fd. */
  std::vector<int> dups;
  {
    std::lock_guard<std::mutex> guard(workersMtx_);
    for (int fd : activeFds_) {
      int d = dup(fd);
      if (d >= 0) {
        dups.push_back(d);
      } else {
        shutdown(fd, SHUT_RDWR); /* linger-off: never blocks */
      }
    }
  }
  for (int d : dups) {
    shutdown(d, SHUT_RDWR);
    close(d);
  }
  /* Worker production barrier: if the accept loop is still
   * running on another thread (stop called concurrently), join
   * its tracked handle first - after the join no registration can
   * follow the live-count wait. runThreaded-on-caller-thread has
   * no joinable handle here; in that case the loop already exited
   * before stop() was called. */
  if (acceptLoop_.joinable()) {
    acceptLoop_.join();
  }
  /* All workers are detached: wait for the live count to reach
   * zero. The FD shutdown above unblocks any worker stuck in a
   * socket call, and each worker decrements the count only after
   * removing its fd - so stop() returns with the registry empty
   * and no thread touching server state. */
  {
    std::unique_lock<std::mutex> lock(workersMtx_);
    workerDoneCv_.wait(lock, [this] {
      return liveWorkers_ == 0;
    });
  }
}

void HttpServer::handleConnection(int client, bool closeFd) {
  /* Absolute receive deadline (request bytes only). */
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  uint64_t deadlineMs =
    static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) +
    5000;
  std::string raw;
  char buf[65536];
  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    auto now2 = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t now2Ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now2).count());
    if (now2Ms >= deadlineMs) {
      if (closeFd) {
        close(client);
      }
      return;
    }
    uint64_t remain = deadlineMs - now2Ms;
    if (remain > 5000) {
      remain = 5000;
    }
    timeval tv{};
    tv.tv_sec = static_cast<long>(remain / 1000);
    tv.tv_usec = static_cast<long>((remain % 1000) * 1000);
    if (remain == 0) {
      tv.tv_usec = 1000;
    }
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(client, buf, sizeof(buf), 0);
    if (n <= 0) {
      if (n < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (closeFd) {
        close(client);
      }
      return;
    }
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > kMaxHeaderBytes) {
      if (closeFd) {
        close(client);
      }
      return;
    }
    headerEnd = raw.find("\r\n\r\n");
  }
  /* Body per Content-Length (control requests carry zero). */
  size_t headerLen = headerEnd + 4;
  size_t contentLen = 0;
  {
    HttpRequest head = parseRequest(raw.substr(0, headerLen));
    auto it = head.headers.find("content-length");
    if (it != head.headers.end()) {
      contentLen = static_cast<size_t>(
        strtoull(it->second.c_str(), nullptr, 10));
      if (contentLen > kMaxBodyBytes) {
        if (closeFd) {
          close(client);
        }
        return;
      }
    }
  }
  /* Body receive honors the same absolute deadline as the header:
   * a slow trickle cannot extend it, and a short body is rejected
   * rather than half-delivered. */
  bool bodyComplete = false;
  while (raw.size() - headerLen < contentLen) {
    auto nowB = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t nowBMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(nowB).count());
    if (nowBMs >= deadlineMs) {
      break;
    }
    uint64_t remain = deadlineMs - nowBMs;
    if (remain > 5000) {
      remain = 5000;
    }
    timeval tv{};
    tv.tv_sec = static_cast<long>(remain / 1000);
    tv.tv_usec = static_cast<long>((remain % 1000) * 1000);
    if (remain == 0) {
      tv.tv_usec = 1000;
    }
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(client, buf, sizeof(buf), 0);
    if (n <= 0) {
      if (n < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      break;
    }
    raw.append(buf, static_cast<size_t>(n));
  }
  bodyComplete = raw.size() - headerLen >= contentLen;
  if (!bodyComplete) {
    if (closeFd) {
      close(client);
    }
    return;
  }

  HttpRequest req = parseRequest(raw.substr(0, headerLen));
  req.body = raw.substr(headerLen);
  HttpResponse resp = handler_(req);

  /* Response transmission deadline: 5s from handler completion. */
  auto now3 = std::chrono::steady_clock::now().time_since_epoch();
  uint64_t txDeadline =
    static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now3).count()) +
    5000;
  bool sentOk = false;
  try {
    std::string out = serializeResponse(resp);
    sentOk = sendAll(client, out, txDeadline);
  } catch (...) {
    sentOk = false;
  }
  /* Finalizer: exactly once, exceptions contained. */
  auto fin = std::move(resp.afterSend);
  resp.afterSend = nullptr;
  if (fin) {
    try {
      fin(sentOk);
    } catch (...) {
      /* logged upstream if needed */
    }
  }
  if (closeFd) {
    close(client);
  }
}

HttpRequest parseRequest(const std::string& raw) {
  HttpRequest req;
  std::istringstream iss(raw);
  std::string line;
  if (!std::getline(iss, line)) {
    return req;
  }
  {
    std::istringstream ls(line);
    ls >> req.method >> req.path;
  }
  size_t blockEnd = raw.find("\r\n\r\n");
  req.rawHeaders = blockEnd == std::string::npos ? raw
                                                 : raw.substr(0, blockEnd);
  while (std::getline(iss, line) && line != "\r" && !line.empty()) {
    if (line.back() == '\r') {
      line.pop_back();
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = toLower(trim(line.substr(0, colon)));
    std::string val = trim(line.substr(colon + 1));
    req.headers[key] = val;
  }
  std::ostringstream body;
  body << iss.rdbuf();
  req.body = body.str();
  return req;
}

std::string serializeResponse(const HttpResponse& resp) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << resp.status << " " << reasonPhrase(resp.status)
      << "\r\n";
  oss << "Content-Length: " << resp.body.size() << "\r\n";
  for (const auto& [k, v] : resp.headers) {
    oss << k << ": " << v << "\r\n";
  }
  oss << "\r\n";
  oss << resp.body;
  return oss.str();
}

} // namespace hipobj::test
