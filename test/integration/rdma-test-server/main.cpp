/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RC-native S3-over-RDMA integration test server.
 *
 * Accepts GET/PUT with x-amz-rdma-token, performs RC RDMA transfers,
 * and returns x-amz-rdma-reply with an optional peer token for the
 * client-side RC handshake.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hipobj-parse.h"
#include "hipobj-print.h"
#include "hipobj-span.h"
#include "http_server.h"
#include "ibv-wrapper.h"
#include "rdma_server.h"
#include "token_header.h"

namespace {

/* The port the server listens on when the arguments don't give one */
constexpr int kDefaultPort = 9000;

std::string
objectKey(const std::string &path)
{
    if (!path.empty() && path[0] == '/') {
        return path.substr(1);
    }
    return path;
}

void
usage(const std::string &prog)
{
    hipObj::printTo(stderr, "usage: {} [port]\n", prog);
}

} // namespace

int
main(int argc, char *argv[])
{
    const std::span<char *>        argvSpan = hipObj::argSpan(argc, argv);
    const std::vector<std::string> args(argvSpan.begin(), argvSpan.end());
    const std::string              prog = args.empty() ? "hipobj-rdma-test-server" : args[0];
    int                            port = kDefaultPort;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg[0] == '-') {
            hipObj::printTo(stderr, "hipobj-rdma-test-server: unknown option {}\n", arg);
            usage(prog);
            return 2;
        }
        const std::optional<uint16_t> value = hipObj::parseNumber<uint16_t>(arg);
        if (!value || *value == 0) {
            hipObj::printTo(stderr, "hipobj-rdma-test-server: invalid port {}\n", arg);
            usage(prog);
            return 2;
        }
        port = *value;
    }

    /* Declared before the server, so it outlives it. Null, after a
     * message, when libibverbs can't be loaded. */
    const std::unique_ptr<hipObj::LoadedVerbs> verbs = hipObj::loadLibibverbs();
    hipobj::test::RdmaTestServer               rdma(verbs.get());
    if (!rdma.isReady()) {
        hipObj::printTo(stderr, "hipobj-rdma-test-server: RDMA not available — running in "
                                "HTTP-only mode\n");
    }

    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex                                  objects_mu;

    hipobj::test::HttpServer server(port);
    server.setHandler([&](const hipobj::test::HttpRequest &req) -> hipobj::test::HttpResponse {
        hipobj::test::HttpResponse resp;
        if (req.method == "GET" && req.path == "/health") {
            resp.status = hipobj::test::kHttpOk;
            resp.body   = "ok";
            return resp;
        }

        // S3 GetBucketLocation — minio-cpp issues this before every transfer
        // to resolve the effective region.  Return a minimal XML body so the
        // bridge can proceed to the RDMA path without a real S3 stack.
        if (req.method == "GET" && req.path.find("?location") != std::string::npos) {
            resp.status                  = hipobj::test::kHttpOk;
            resp.headers["Content-Type"] = "application/xml";
            resp.body                    = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                           "<LocationConstraint "
                                           "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                                           "us-east-1</LocationConstraint>";
            return resp;
        }

        const std::string key     = objectKey(req.path);
        const auto        tokenIt = req.headers.find("x-amz-rdma-token");

        // HTTP-only mode: RDMA token absent or RDMA not available.
        if (tokenIt == req.headers.end() || !rdma.isReady()) {
            if (req.method == "PUT") {
                std::scoped_lock<std::mutex> lk(objects_mu);
                objects[key]         = std::vector<uint8_t>(req.body.begin(), req.body.end());
                resp.status          = hipobj::test::kHttpOk;
                resp.headers["etag"] = "\"test\"";
                return resp;
            }
            if (req.method == "GET") {
                std::scoped_lock<std::mutex> lk(objects_mu);
                const auto                   it = objects.find(key);
                if (it == objects.end()) {
                    resp.status = hipobj::test::kHttpNotFound;
                    resp.body   = "not found";
                    return resp;
                }
                resp.status = hipobj::test::kHttpOk;
                resp.body   = std::string(it->second.begin(), it->second.end());
                return resp;
            }
            resp.status = hipobj::test::kHttpMethodNotAllowed;
            resp.body   = "method not allowed";
            return resp;
        }

        // Reaching here: token present and RDMA ready — use RDMA path.
        const std::optional<hipobj::test::TokenHeader> token =
            hipobj::test::parseTokenHeader(tokenIt->second);
        if (!token) {
            resp.status = hipobj::test::kHttpBadRequest;
            resp.body   = "malformed x-amz-rdma-token";
            return resp;
        }
        if (req.method == "PUT") {
            std::optional<hipobj::test::RdmaTestServer::ReadResult> read =
                rdma.rdmaReadFromClient(*token, req.contentLength);
            if (!read) {
                resp.status = hipobj::test::kHttpInternalServerError;
                resp.body   = "RDMA PUT failed";
                return resp;
            }
            {
                std::scoped_lock<std::mutex> lk(objects_mu);
                objects[key] = std::move(read->data);
            }
            resp.status                      = hipobj::test::kHttpOk;
            resp.headers["x-amz-rdma-reply"] = std::move(read->replyHeader);
            resp.headers["etag"]             = "\"test\"";
            return resp;
        }

        if (req.method == "GET") {
            std::vector<uint8_t> data;
            {
                std::scoped_lock<std::mutex> lk(objects_mu);
                const auto                   it = objects.find(key);
                if (it == objects.end()) {
                    resp.status = hipobj::test::kHttpNotFound;
                    resp.body   = "not found";
                    return resp;
                }
                data = it->second;
            }
            std::optional<std::string> replyHeader = rdma.rdmaWriteToClient(*token, data);
            if (!replyHeader) {
                resp.status = hipobj::test::kHttpInternalServerError;
                resp.body   = "RDMA GET failed";
                return resp;
            }
            resp.status                                  = hipobj::test::kHttpOk;
            resp.headers["x-amz-rdma-reply"]             = std::move(*replyHeader);
            resp.headers["x-amz-rdma-bytes-transferred"] = std::to_string(data.size());
            return resp;
        }

        resp.status = hipobj::test::kHttpMethodNotAllowed;
        resp.body   = "method not allowed";
        return resp;
    });

    // The threaded server receives large HTTP request bodies (e.g. HTTP-only
    // PUT payloads) in full rather than truncating them at a single recv().
    /* "listening on port N" is what every caller waits for before it starts
     * sending. Printing it without checking made a dead server look ready. */
    if (!server.startThreaded()) {
        hipObj::printTo(stderr, "hipobj-rdma-test-server: not listening on port {}\n", port);
        return 1;
    }
    hipObj::printTo(stdout, "hipobj-rdma-test-server listening on port {}\n", port);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
