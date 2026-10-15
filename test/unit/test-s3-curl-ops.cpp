/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the example S3 curl helper (s3_curl_ops).
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "hipobj-address.h"
#include "hipobj-sockaddr.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "s3_curl_ops.h"
#include "unique-fd.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

constexpr auto kServeDeadline = std::chrono::seconds(10);

struct ServeResult {
    bool accepted        = false;
    bool requestComplete = false;
    bool responseSent    = false;
    int  err             = 0;

    [[nodiscard]] bool ok() const
    {
        return accepted && requestComplete && responseSent && err == 0;
    }
};

// Returns the milliseconds left before deadline, clamped to [0, INT_MAX]
// so it can be passed straight to poll()'s int timeout
int
remainingMs(const std::chrono::steady_clock::time_point &deadline)
{
    const auto left = deadline - std::chrono::steady_clock::now();
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::clamp<decltype(ms)>(ms, 0, std::numeric_limits<int>::max()));
}

// Single-connection loopback HTTP stub. It accepts one connection,
// reads one request, writes a canned response, and closes. All waits
// are nonblocking against an absolute deadline, so the worker thread
// always terminates within kServeDeadline even if the client never
// connects or disappears mid-transfer.
class StubHttpServer {
public:
    explicit StubHttpServer(std::string_view response)
        : listenFd_(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)), response_(response)
    {
        if (!listenFd_) {
            throw std::runtime_error("socket() failed");
        }
        int one = 1;
        setsockopt(listenFd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr {};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (bind(listenFd_.get(), hipObj::asSockaddr(addr), sizeof(addr)) != 0 ||
            listen(listenFd_.get(), 1) != 0) {
            throw std::runtime_error("bind/listen failed");
        }

        socklen_t slen = sizeof(addr);
        if (getsockname(listenFd_.get(), hipObj::asSockaddr(addr), &slen) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        endpoint_ = "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(ntohs(addr.sin_port)));

        worker_ = std::thread([this] { serve(); });
    }

    ~StubHttpServer()
    {
        joinWorker();
    }

    // The worker thread holds this
    StubHttpServer(const StubHttpServer &)            = delete;
    StubHttpServer &operator=(const StubHttpServer &) = delete;
    StubHttpServer(StubHttpServer &&)                 = delete;
    StubHttpServer &operator=(StubHttpServer &&)      = delete;

    // Usable right after construction; the value never changes.
    [[nodiscard]] const std::string &endpoint() const
    {
        return endpoint_;
    }

    // Joins the worker and reports how the single exchange went. Safe to
    // call more than once.
    ServeResult waitDone()
    {
        joinWorker();
        return result_;
    }

    // Only valid after waitDone() has joined the worker.
    [[nodiscard]] const std::string &lastRequest() const
    {
        return request_;
    }

private:
    void joinWorker()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Runs the single-connection exchange against one absolute deadline:
    // accept a connection, read until the request headers are terminated
    // (or 8 KiB has been read), then write the canned response. Any poll
    // timeout, I/O error, or the deadline expiring ends the exchange early,
    // leaving result_ to report what happened.
    void serve()
    {
        const auto deadline = std::chrono::steady_clock::now() + kServeDeadline;

        // --- Accept phase: wait for a connection, retrying on EINTR and the
        // transient accept4() errors a nonblocking listen socket can produce.
        hipObj::UniqueFd fd;
        for (;;) {
            if (remainingMs(deadline) == 0) {
                result_.err = ETIMEDOUT;
                return;
            }
            struct pollfd pfd;
            pfd.fd       = listenFd_.get();
            pfd.events   = POLLIN;
            pfd.revents  = 0;
            const int pr = poll(&pfd, 1, remainingMs(deadline));
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
            fd.reset(accept4(listenFd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (fd) {
                break;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == EPROTO ||
                errno == ECONNABORTED) {
                continue;
            }
            result_.err = errno;
            return;
        }
        result_.accepted = true;

        // --- Read phase: accumulate bytes until the header terminator shows
        // up or at least 8 KiB has been read. The size is checked before each
        // recv(), so the request can overshoot that threshold by up to one
        // buffer. A clean disconnect just ends the loop; requestComplete
        // below reports whether the terminator was actually seen.
        std::array<char, 4096> buf{};
        while (request_.find("\r\n\r\n") == std::string::npos && request_.size() < 8192) {
            if (remainingMs(deadline) == 0) {
                result_.err = ETIMEDOUT;
                return;
            }
            struct pollfd pfd;
            pfd.fd       = fd.get();
            pfd.events   = POLLIN;
            pfd.revents  = 0;
            const int pr = poll(&pfd, 1, remainingMs(deadline));
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
            const ssize_t n = recv(fd.get(), buf.data(), buf.size(), 0);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                result_.err = errno;
                return;
            }
            if (n == 0) {
                break; // client hung up before sending complete headers
            }
            request_.append(buf.data(), static_cast<size_t>(n));
        }
        result_.requestComplete = request_.find("\r\n\r\n") != std::string::npos;

        // --- Write phase: send the canned response, looping on partial
        // writes the same way the read phase loops on partial reads.
        size_t sent = 0;
        while (sent < response_.size()) {
            if (remainingMs(deadline) == 0) {
                result_.err = ETIMEDOUT;
                return;
            }
            struct pollfd pfd;
            pfd.fd       = fd.get();
            pfd.events   = POLLOUT;
            pfd.revents  = 0;
            const int pr = poll(&pfd, 1, remainingMs(deadline));
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
            const std::string_view rest = std::string_view(response_).substr(sent);
            const ssize_t          n    = send(fd.get(), rest.data(), rest.size(), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                result_.err = errno;
                return;
            }
            sent += static_cast<size_t>(n);
        }
        result_.responseSent = true;
    }

    hipObj::UniqueFd listenFd_;
    std::string      response_;
    std::string      endpoint_{};
    std::string      request_{};
    ServeResult      result_{};
    std::thread      worker_{};
};

constexpr std::string_view kToken = "rdma-test-token";

constexpr std::string_view kR500         = "HTTP/1.0 500 Internal Server Error\r\n"
                                           "X-Amz-Rdma-Reply: ok\r\n"
                                           "\r\n";
constexpr std::string_view kR204         = "HTTP/1.0 204 No Content\r\n"
                                           "X-Amz-Rdma-Reply: ok\r\n"
                                           "\r\n";
constexpr std::string_view kR299         = "HTTP/1.0 299 Unknown\r\n"
                                           "X-Amz-Rdma-Reply: ok\r\n"
                                           "\r\n";
constexpr std::string_view kR300         = "HTTP/1.0 300 Multiple Choices\r\n"
                                           "X-Amz-Rdma-Reply: ok\r\n"
                                           "\r\n";
constexpr std::string_view kR200NoReply  = "HTTP/1.0 200 OK\r\n"
                                           "Content-Length: 0\r\n"
                                           "\r\n";
constexpr std::string_view kR200ErrMixed = "HTTP/1.0 200 OK\r\n"
                                           "X-Amz-Rdma-Reply: err\r\n"
                                           "\r\n";
constexpr std::string_view kR200OkCaps   = "HTTP/1.0 200 OK\r\n"
                                           "X-AMZ-RDMA-REPLY: ok\r\n"
                                           "\r\n";

// ctx keeps endpoint.c_str(), so the string must outlive ctx.
void
fillCtx(hipObjS3CurlCtx *ctx, const std::string &endpoint)
{
    ctx->endpoint   = endpoint.c_str();
    ctx->objectSize = 4096;
    ctx->devPtr     = hipObj::addressToPointer<const void *>(0x1000);
}

// Rejects temporaries (including ones converted from a char array),
// which would leave ctx->endpoint dangling.
void fillCtx(hipObjS3CurlCtx *ctx, std::string &&endpoint) = delete;

// Runs one request against a fresh stub server and checks both that the
// stub exchange itself completed and that SendRequest returned the
// expected value. Checking the stub result keeps a broken stub from
// masquerading as a passing negative test.
void
expectSendResult(std::string_view response, int expectedRc)
{
    StubHttpServer  server(response);
    hipObjS3CurlCtx ctx{};
    fillCtx(&ctx, server.endpoint());
    const int actualRc = hipObjS3CurlSendRequest(&ctx, kToken.data(), kToken.size());
    ASSERT_TRUE(server.waitDone().ok());
    EXPECT_EQ(expectedRc, actualRc);
}

class CurlGlobalEnv : public ::testing::Environment {
public:
    void SetUp() override
    {
        // Keep loopback requests away from any configured proxy. The test
        // sets these itself, rather than relying on ctest to, so they apply
        // when the program is run directly too. setenv() isn't thread-safe,
        // but no other threads exist yet.
        // NOLINTBEGIN(concurrency-mt-unsafe)
        setenv("no_proxy", "127.0.0.1", 1);
        setenv("NO_PROXY", "127.0.0.1", 1);
        // NOLINTEND(concurrency-mt-unsafe)
        const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            initialized_ = false;
            FAIL() << "curl_global_init failed: " << static_cast<int>(rc);
        }
        initialized_ = true;
    }

    void TearDown() override
    {
        if (initialized_) {
            curl_global_cleanup();
        }
    }

private:
    bool initialized_ = false;
};

const ::testing::Environment *const kCurlEnv = ::testing::AddGlobalTestEnvironment(new CurlGlobalEnv);

} // namespace

TEST(S3CurlOps, Http500WithReplyHeaderFails)
{
    expectSendResult(kR500, -1);
}

TEST(S3CurlOps, Http204WithReplyHeaderSucceeds)
{
    expectSendResult(kR204, 0);
}

TEST(S3CurlOps, Http299WithReplyHeaderSucceeds)
{
    expectSendResult(kR299, 0);
}

TEST(S3CurlOps, Http300WithReplyHeaderFails)
{
    expectSendResult(kR300, -1);
}

TEST(S3CurlOps, MissingReplyHeaderFails)
{
    expectSendResult(kR200NoReply, -1);
}

TEST(S3CurlOps, ReplyHeaderMixedCase)
{
    StubHttpServer  server(kR200ErrMixed);
    hipObjS3CurlCtx ctx{};
    fillCtx(&ctx, server.endpoint());
    const int rc = hipObjS3CurlSendRequest(&ctx, kToken.data(), kToken.size());
    ASSERT_TRUE(server.waitDone().ok());
    EXPECT_EQ(0, rc);

    std::array<char, 16> reply{};
    size_t               len = reply.size();
    EXPECT_EQ(0, hipObjS3CurlRecvReply(&ctx, reply.data(), &len));
    EXPECT_EQ(3U, len);
    EXPECT_EQ(hipObj::asStringView(std::span(reply).first(3)), "err");
}

TEST(S3CurlOps, ReplyHeaderAllCaps)
{
    StubHttpServer  server(kR200OkCaps);
    hipObjS3CurlCtx ctx{};
    fillCtx(&ctx, server.endpoint());
    const int rc = hipObjS3CurlSendRequest(&ctx, kToken.data(), kToken.size());
    ASSERT_TRUE(server.waitDone().ok());
    EXPECT_EQ(0, rc);

    std::array<char, 16> reply{};
    size_t               len = reply.size();
    EXPECT_EQ(0, hipObjS3CurlRecvReply(&ctx, reply.data(), &len));
    EXPECT_EQ(2U, len);
    EXPECT_EQ(hipObj::asStringView(std::span(reply).first(2)), "ok");
}

TEST(S3CurlOps, RequestContract)
{
    StubHttpServer  server(kR200OkCaps);
    hipObjS3CurlCtx ctx{};
    fillCtx(&ctx, server.endpoint());
    const int rc = hipObjS3CurlSendRequest(&ctx, kToken.data(), kToken.size());
    ASSERT_TRUE(server.waitDone().ok());
    EXPECT_EQ(0, rc);

    const std::string &req = server.lastRequest();
    EXPECT_NE(std::string::npos, req.find("GET /test/object HTTP/1.1"));
    EXPECT_NE(std::string::npos, req.find("x-amz-rdma-token: " + std::string(kToken) + "\r\n"));
}

TEST(S3CurlOps, ConnectionRefusedFails)
{
    // Bind a socket without listening and hold it open for the duration
    // of the request so the port cannot be taken over; connections to it
    // are refused deterministically.
    const hipObj::UniqueFd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    ASSERT_TRUE(fd);
    struct sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    ASSERT_EQ(0, bind(fd.get(), hipObj::asSockaddr(addr), sizeof(addr)));
    socklen_t slen = sizeof(addr);
    ASSERT_EQ(0, getsockname(fd.get(), hipObj::asSockaddr(addr), &slen));
    const std::string url = "http://127.0.0.1:" + std::to_string(static_cast<unsigned>(ntohs(addr.sin_port)));

    hipObjS3CurlCtx ctx{};
    fillCtx(&ctx, url);
    const int rc = hipObjS3CurlSendRequest(&ctx, kToken.data(), kToken.size());
    EXPECT_EQ(-1, rc);
}

TEST(S3CurlOps, SendRequestNullCtx)
{
    EXPECT_EQ(-1, hipObjS3CurlSendRequest(nullptr, kToken.data(), kToken.size()));
}

TEST(S3CurlOps, SendRequestNullToken)
{
    hipObjS3CurlCtx ctx{};
    EXPECT_EQ(-1, hipObjS3CurlSendRequest(&ctx, nullptr, 0));
}

TEST(S3CurlOps, RecvReplyTooSmall)
{
    hipObjS3CurlCtx ctx{};
    std::ranges::copy(std::string_view("ok"), std::span(ctx.lastReply).begin());
    std::array<char, 8> reply{};
    size_t              len = 1;
    EXPECT_EQ(-1, hipObjS3CurlRecvReply(&ctx, reply.data(), &len));
}

TEST(S3CurlOps, RecvReplyExactSize)
{
    hipObjS3CurlCtx ctx{};
    std::ranges::copy(std::string_view("ok"), std::span(ctx.lastReply).begin());
    std::array<char, 2> reply{};
    size_t              len = reply.size();
    EXPECT_EQ(0, hipObjS3CurlRecvReply(&ctx, reply.data(), &len));
    EXPECT_EQ(2U, len);
    EXPECT_EQ(hipObj::asStringView(reply), "ok");
}

TEST(S3CurlOps, RecvReplyNullArgs)
{
    hipObjS3CurlCtx     ctx{};
    std::array<char, 8> reply{};
    size_t              len = reply.size();
    EXPECT_EQ(-1, hipObjS3CurlRecvReply(nullptr, reply.data(), &len));
    EXPECT_EQ(-1, hipObjS3CurlRecvReply(&ctx, nullptr, &len));
    EXPECT_EQ(-1, hipObjS3CurlRecvReply(&ctx, reply.data(), nullptr));
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
