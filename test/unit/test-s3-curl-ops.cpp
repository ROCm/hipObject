/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the example S3 curl helper (s3_curl_ops).
 */

#include <errno.h>
#include <string.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "s3_curl_ops.h"

namespace {

constexpr auto kServeDeadline = std::chrono::seconds(10);

struct ServeResult {
  bool accepted = false;
  bool requestComplete = false;
  bool responseSent = false;
  int err = 0;

  bool ok() const {
    return accepted && requestComplete && responseSent && err == 0;
  }
};

long remainingMs(const std::chrono::steady_clock::time_point& deadline) {
  auto left = deadline - std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left);
  return ms.count() < 0 ? 0 : static_cast<long>(ms.count());
}

// Single-connection loopback HTTP stub. It accepts one connection,
// reads one request, writes a canned response, and closes. All waits
// are nonblocking against an absolute deadline, so the worker thread
// always terminates within kServeDeadline even if the client never
// connects or disappears mid-transfer.
class StubHttpServer {
public:
  explicit StubHttpServer(std::string response)
    : response_(std::move(response)) {
    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenFd_ < 0) {
      throw std::runtime_error("socket() failed");
    }
    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0 ||
        listen(listenFd_, 1) != 0) {
      close(listenFd_);
      listenFd_ = -1;
      throw std::runtime_error("bind/listen failed");
    }

    socklen_t slen = sizeof(addr);
    if (getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&addr),
                    &slen) != 0) {
      close(listenFd_);
      listenFd_ = -1;
      throw std::runtime_error("getsockname failed");
    }
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u",
             static_cast<unsigned>(ntohs(addr.sin_port)));
    endpoint_ = url;

    worker_ = std::thread([this] {
      serve();
    });
  }

  ~StubHttpServer() {
    joinWorker();
    if (listenFd_ >= 0) {
      close(listenFd_);
    }
  }

  // Usable right after construction; the value never changes.
  const std::string& endpoint() const {
    return endpoint_;
  }

  // Joins the worker and reports how the single exchange went. Safe to
  // call more than once.
  ServeResult waitDone() {
    joinWorker();
    return result_;
  }

  // Only valid after waitDone() has joined the worker.
  const std::string& lastRequest() const {
    return request_;
  }

private:
  void joinWorker() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void fail(int err, int fd) {
    result_.err = err;
    if (fd >= 0) {
      close(fd);
    }
  }

  void serve() {
    const auto deadline = std::chrono::steady_clock::now() + kServeDeadline;

    int fd = -1;
    for (;;) {
      if (remainingMs(deadline) == 0) {
        result_.err = ETIMEDOUT;
        return;
      }
      struct pollfd pfd;
      pfd.fd = listenFd_;
      pfd.events = POLLIN;
      pfd.revents = 0;
      int pr = poll(&pfd, 1, remainingMs(deadline));
      if (pr < 0) {
        if (errno == EINTR) {
          continue;
        }
        result_.err = errno;
        return;
      }
      if (pr == 0) {
        result_.err = ETIMEDOUT;
        return;
      }
      fd = accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK);
      if (fd >= 0) {
        break;
      }
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EPROTO || errno == ECONNABORTED) {
        continue;
      }
      result_.err = errno;
      return;
    }
    result_.accepted = true;

    char buf[4096];
    while (request_.find("\r\n\r\n") == std::string::npos &&
           request_.size() < 8192) {
      if (remainingMs(deadline) == 0) {
        fail(ETIMEDOUT, fd);
        return;
      }
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      int pr = poll(&pfd, 1, remainingMs(deadline));
      if (pr < 0) {
        if (errno == EINTR) {
          continue;
        }
        fail(errno, fd);
        return;
      }
      if (pr == 0) {
        fail(ETIMEDOUT, fd);
        return;
      }
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        fail(errno, fd);
        return;
      }
      if (n == 0) {
        break; // client hung up before sending complete headers
      }
      request_.append(buf, static_cast<size_t>(n));
    }
    result_.requestComplete = request_.find("\r\n\r\n") != std::string::npos;

    size_t sent = 0;
    while (sent < response_.size()) {
      if (remainingMs(deadline) == 0) {
        fail(ETIMEDOUT, fd);
        return;
      }
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      int pr = poll(&pfd, 1, remainingMs(deadline));
      if (pr < 0) {
        if (errno == EINTR) {
          continue;
        }
        fail(errno, fd);
        return;
      }
      if (pr == 0) {
        fail(ETIMEDOUT, fd);
        return;
      }
      ssize_t n = send(fd, response_.data() + sent, response_.size() - sent,
                       MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        fail(errno, fd);
        return;
      }
      sent += static_cast<size_t>(n);
    }
    result_.responseSent = true;
    close(fd); // close() is not retried on EINTR
  }

  int listenFd_ = -1;
  std::string response_;
  std::string endpoint_;
  std::string request_;
  ServeResult result_;
  std::thread worker_;
};

const char kToken[] = "0123456789abcdef";
constexpr size_t kTokenLen = sizeof(kToken) - 1;

const char kR500[] = "HTTP/1.0 500 Internal Server Error\r\n"
                     "X-Amz-Rdma-Reply: ok\r\n"
                     "\r\n";
const char kR204[] = "HTTP/1.0 204 No Content\r\n"
                     "X-Amz-Rdma-Reply: ok\r\n"
                     "\r\n";
const char kR299[] = "HTTP/1.0 299 Unknown\r\n"
                     "X-Amz-Rdma-Reply: ok\r\n"
                     "\r\n";
const char kR300[] = "HTTP/1.0 300 Multiple Choices\r\n"
                     "X-Amz-Rdma-Reply: ok\r\n"
                     "\r\n";
const char kR200NoReply[] = "HTTP/1.0 200 OK\r\n"
                            "Content-Length: 0\r\n"
                            "\r\n";
const char kR200ErrMixed[] = "HTTP/1.0 200 OK\r\n"
                             "X-Amz-Rdma-Reply: err\r\n"
                             "\r\n";
const char kR200OkCaps[] = "HTTP/1.0 200 OK\r\n"
                           "X-AMZ-RDMA-REPLY: ok\r\n"
                           "\r\n";

void fillCtx(hipObjS3CurlCtx* ctx, const std::string& endpoint) {
  ctx->endpoint = endpoint.c_str();
  ctx->objectSize = 4096;
  ctx->devPtr = reinterpret_cast<const void*>(0x1000);
}

// Runs one request against a fresh stub server and checks both that the
// stub exchange itself completed and that SendRequest returned the
// expected value. Checking the stub result keeps a broken stub from
// masquerading as a passing negative test.
void expectSendResult(const std::string& response, int expectedRc) {
  StubHttpServer server(response);
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  fillCtx(&ctx, server.endpoint());
  int rc = hipObjS3CurlSendRequest(&ctx, kToken, kTokenLen);
  ASSERT_TRUE(server.waitDone().ok());
  EXPECT_EQ(expectedRc, rc);
}

class CurlGlobalEnv : public ::testing::Environment {
public:
  void SetUp() override {
    // Keep loopback requests away from any configured proxy. glibc
    // setenv() is not thread-safe; no worker threads exist yet.
    setenv("no_proxy", "127.0.0.1", 1);
    setenv("NO_PROXY", "127.0.0.1", 1);
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
      initialized_ = false;
      FAIL() << "curl_global_init failed: " << static_cast<int>(rc);
      return;
    }
    initialized_ = true;
  }

  void TearDown() override {
    if (initialized_) {
      curl_global_cleanup();
    }
  }

private:
  bool initialized_ = false;
};

const ::testing::Environment* const kCurlEnv =
  ::testing::AddGlobalTestEnvironment(new CurlGlobalEnv);

} // namespace

TEST(S3CurlOps, Http500WithReplyHeaderFails) {
  expectSendResult(kR500, -1);
}

TEST(S3CurlOps, Http204WithReplyHeaderSucceeds) {
  expectSendResult(kR204, 0);
}

TEST(S3CurlOps, Http299WithReplyHeaderSucceeds) {
  expectSendResult(kR299, 0);
}

TEST(S3CurlOps, Http300WithReplyHeaderFails) {
  expectSendResult(kR300, -1);
}

TEST(S3CurlOps, MissingReplyHeaderFails) {
  expectSendResult(kR200NoReply, -1);
}

TEST(S3CurlOps, ReplyHeaderMixedCase) {
  StubHttpServer server(kR200ErrMixed);
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  fillCtx(&ctx, server.endpoint());
  int rc = hipObjS3CurlSendRequest(&ctx, kToken, kTokenLen);
  ASSERT_TRUE(server.waitDone().ok());
  EXPECT_EQ(0, rc);

  char reply[16];
  memset(reply, 0, sizeof(reply));
  size_t len = sizeof(reply);
  EXPECT_EQ(0, hipObjS3CurlRecvReply(&ctx, reply, &len));
  EXPECT_EQ(3u, len);
  EXPECT_EQ(0, memcmp(reply, "err", 3));
}

TEST(S3CurlOps, ReplyHeaderAllCaps) {
  StubHttpServer server(kR200OkCaps);
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  fillCtx(&ctx, server.endpoint());
  int rc = hipObjS3CurlSendRequest(&ctx, kToken, kTokenLen);
  ASSERT_TRUE(server.waitDone().ok());
  EXPECT_EQ(0, rc);

  char reply[16];
  memset(reply, 0, sizeof(reply));
  size_t len = sizeof(reply);
  EXPECT_EQ(0, hipObjS3CurlRecvReply(&ctx, reply, &len));
  EXPECT_EQ(2u, len);
  EXPECT_EQ(0, memcmp(reply, "ok", 2));
}

TEST(S3CurlOps, RequestContract) {
  StubHttpServer server(kR200OkCaps);
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  fillCtx(&ctx, server.endpoint());
  int rc = hipObjS3CurlSendRequest(&ctx, kToken, kTokenLen);
  ASSERT_TRUE(server.waitDone().ok());
  EXPECT_EQ(0, rc);

  const std::string& req = server.lastRequest();
  EXPECT_NE(std::string::npos, req.find("GET /test/object HTTP/1.1"));
  EXPECT_NE(std::string::npos, req.find("x-amz-rdma-token:"));
}

TEST(S3CurlOps, ConnectionRefusedFails) {
  // Bind a socket without listening and hold it open for the duration
  // of the request so the port cannot be taken over; connections to it
  // are refused deterministically.
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(0,
            bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  socklen_t slen = sizeof(addr);
  ASSERT_EQ(0,
            getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &slen));
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%u",
           static_cast<unsigned>(ntohs(addr.sin_port)));

  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  fillCtx(&ctx, url);
  int rc = hipObjS3CurlSendRequest(&ctx, kToken, kTokenLen);
  EXPECT_EQ(-1, rc);
  close(fd);
}

TEST(S3CurlOps, SendRequestNullCtx) {
  EXPECT_EQ(-1, hipObjS3CurlSendRequest(nullptr, kToken, kTokenLen));
}

TEST(S3CurlOps, SendRequestNullToken) {
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  EXPECT_EQ(-1, hipObjS3CurlSendRequest(&ctx, nullptr, 0));
}

TEST(S3CurlOps, RecvReplyTooSmall) {
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  memcpy(ctx.lastReply, "ok", sizeof("ok"));
  char reply[8];
  size_t len = 1;
  EXPECT_EQ(-1, hipObjS3CurlRecvReply(&ctx, reply, &len));
}

TEST(S3CurlOps, RecvReplyExactSize) {
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  memcpy(ctx.lastReply, "ok", sizeof("ok"));
  char reply[2];
  size_t len = 2;
  EXPECT_EQ(0, hipObjS3CurlRecvReply(&ctx, reply, &len));
  EXPECT_EQ(2u, len);
  EXPECT_EQ(0, memcmp(reply, "ok", 2));
}

TEST(S3CurlOps, RecvReplyNullArgs) {
  hipObjS3CurlCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  char reply[8];
  size_t len = sizeof(reply);
  EXPECT_EQ(-1, hipObjS3CurlRecvReply(nullptr, reply, &len));
  EXPECT_EQ(-1, hipObjS3CurlRecvReply(&ctx, nullptr, &len));
  EXPECT_EQ(-1, hipObjS3CurlRecvReply(&ctx, reply, nullptr));
}
