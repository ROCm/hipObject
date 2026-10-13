/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The RDMA test server's HttpServer, and the test clients'
 * parseStatusCode(), both in test/integration/rdma-test-server */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "hipobj-warnings.h"
#include "http_server.h"
#include "http_status.h"
#include "unique-fd.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipobj::test::HttpRequest;
using hipobj::test::HttpResponse;
using hipobj::test::HttpServer;
using hipobj::test::parseStatusCode;

/* How long a client waits for the server to answer and close */
constexpr auto kDeadline = std::chrono::seconds(10);

/* Returns the milliseconds left before deadline, clamped to [0, INT_MAX]
 * so it can be passed straight to poll()'s int timeout */
int
remainingMs(const std::chrono::steady_clock::time_point &deadline)
{
    auto left = deadline - std::chrono::steady_clock::now();
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::clamp<decltype(ms)>(ms, 0, std::numeric_limits<int>::max()));
}

/* An HttpServer on an ephemeral port, with a handler that records each
 * request it's given and answers 200 */
class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        server_.setHandler([this](const HttpRequest &req) {
            const std::scoped_lock<std::mutex> lock(mtx_);
            requests_.push_back(req);
            HttpResponse resp;
            resp.status = 200;
            return resp;
        });
        ASSERT_TRUE(server_.startThreaded());
        sockaddr_in addr{};
        socklen_t   len = sizeof(addr);
        ASSERT_EQ(getsockname(server_.fd(), reinterpret_cast<sockaddr *>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);
    }

    /* Sends request on a new connection and returns everything the server
     * sends back before it closes the connection, or std::nullopt if the
     * exchange fails or the server doesn't close it in time */
    std::optional<std::string> exchange(std::string_view request) const
    {
        const hipObj::UniqueFd fd(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in            addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(port_);
        if (!fd || connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            return std::nullopt;
        }
        size_t sent = 0;
        while (sent < request.size()) {
            const std::string_view rest = std::string_view(request).substr(sent);
            const ssize_t          n    = send(fd.get(), rest.data(), rest.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                return std::nullopt;
            }
            sent += static_cast<size_t>(n);
        }

        const auto  deadline = std::chrono::steady_clock::now() + kDeadline;
        std::string reply;
        char        buf[4096];
        for (;;) {
            pollfd pfd{};
            pfd.fd     = fd.get();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, remainingMs(deadline)) <= 0) {
                return std::nullopt;
            }
            const ssize_t n = recv(fd.get(), buf, sizeof(buf), 0);
            if (n > 0) {
                reply.append(buf, static_cast<size_t>(n));
            }
            /* A reset is a close too: it's what the peer sends if it closes
             * with unread data */
            else if (n == 0 || errno == ECONNRESET) {
                return reply;
            }
            else if (errno != EINTR) {
                return std::nullopt;
            }
        }
    }

    std::vector<HttpRequest> requests()
    {
        const std::scoped_lock<std::mutex> lock(mtx_);
        return requests_;
    }

private:
    std::mutex               mtx_{};
    std::vector<HttpRequest> requests_{};
    uint16_t                 port_ = 0;
    /* Last, so it's destroyed, and its threads stopped, before the members
     * its handler uses */
    HttpServer server_{0};
};

TEST_F(HttpServerTest, PassesValidContentLengthToHandler)
{
    const auto reply = exchange("PUT /bucket/key HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
    ASSERT_TRUE(reply);
    EXPECT_EQ(parseStatusCode(*reply), 200);
    const auto reqs = requests();
    ASSERT_EQ(reqs.size(), 1U);
    EXPECT_EQ(reqs[0].contentLength, 5U);
    EXPECT_EQ(reqs[0].body, "hello");
}

TEST_F(HttpServerTest, MissingContentLengthIsZero)
{
    const auto reply = exchange("GET /bucket/key HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(reply);
    EXPECT_EQ(parseStatusCode(*reply), 200);
    const auto reqs = requests();
    ASSERT_EQ(reqs.size(), 1U);
    EXPECT_EQ(reqs[0].contentLength, 0U);
    EXPECT_TRUE(reqs[0].body.empty());
}

TEST_F(HttpServerTest, ClosesWithoutDispatchOnBadContentLength)
{
    /* Not a number, trailing characters, a sign or prefix, too big for
     * size_t, and over the server's 1 MiB body limit */
    for (const char *value :
         {"", "abc", "5x", "5 5", "-1", "+5", "0x10", "1.5", "99999999999999999999999", "1048577"}) {
        const std::string request =
            std::string("PUT /bucket/key HTTP/1.1\r\nContent-Length: ") + value + "\r\n\r\n";
        const auto reply = exchange(request);
        ASSERT_TRUE(reply) << "\"" << value << "\"";
        EXPECT_TRUE(reply->empty()) << "\"" << value << "\"";
    }
    EXPECT_TRUE(requests().empty());
}

TEST(ParseStatusCode, AcceptsValidStatusLines)
{
    EXPECT_EQ(parseStatusCode("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), 200);
    EXPECT_EQ(parseStatusCode("HTTP/1.1 404 Not Found"), 404);
    EXPECT_EQ(parseStatusCode("HTTP/1.0 100 Continue\r\n"), 100);
    /* The reason phrase can be empty, but the space before it can't */
    EXPECT_EQ(parseStatusCode("HTTP/1.1 599 \r\n"), 599);
}

TEST(ParseStatusCode, RejectsMalformedStatusLines)
{
    for (const char *line : {"",
                             "HTTP/1.1",
                             "HTTP/1.1 200",
                             "HTTP/1.1 200\r\n",
                             "HTTP/1.1 2000 OK",
                             "HTTP/1.1 20 OK",
                             "HTTP/1.1 -12 OK",
                             "HTTP/1.1 +12 OK",
                             "HTTP/1.1 2a0 OK",
                             "HTTP/1.1  200 OK",
                             "HTTP/1.1 099 OK",
                             "HTTP/1.1 600 OK",
                             "XTTP/1.1 200 OK",
                             "HTTP/1.1\r\nX-Y: 1 200 OK",
                             "HTTP/x 200 OK",
                             "HTTP/1x1 200 OK",
                             "HTTP/123 200 OK",
                             "HTTP/1. 200 OK",
                             "HTTP/.1 200 OK",
                             "HTTP/1.1.1 200 OK",
                             "HTTP/2 200 OK"}) {
        EXPECT_FALSE(parseStatusCode(line)) << "\"" << line << "\"";
    }
}

} // namespace
