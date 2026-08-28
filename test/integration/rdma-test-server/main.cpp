/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RC-native S3-over-RDMA integration test server — hipobj-rc-v2 protocol.
 *
 * PREPARE: PUT/GET with x-amz-rdma-token
 *   Server connects QP, returns server token + session ID in x-amz-rdma-reply.
 *
 * READY: PUT/GET with x-amz-rdma-session
 *   Server posts the RDMA operation, polls CQE, returns final status.
 */

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "http_server.h"
#include "rdma_server.h"

namespace {

std::string objectKey(const std::string& path) {
  if (!path.empty() && path[0] == '/') {
    return path.substr(1);
  }
  return path;
}

} // namespace

int main(int argc, char* argv[]) {
  int port = 9000;
  if (argc > 1) {
    port = std::atoi(argv[1]);
  }

  hipobj::test::RdmaTestServer rdma;
  if (!rdma.isReady()) {
    fprintf(stderr,
            "hipobj-rdma-test-server: RDMA not available (libibverbs/NIC)\n");
    return 1;
  }

  std::map<std::string, std::vector<uint8_t>> objects;

  hipobj::test::HttpServer server(port);
  server.setHandler(
    [&](const hipobj::test::HttpRequest& req) -> hipobj::test::HttpResponse {
      hipobj::test::HttpResponse resp;

      if (req.method == "GET" && req.path == "/health") {
        resp.status = 200;
        resp.body = "ok";
        return resp;
      }

      const std::string key = objectKey(req.path);
      auto sessionIt = req.headers.find("x-amz-rdma-session");
      auto tokenIt = req.headers.find("x-amz-rdma-token");

      // --- READY phase: client QP is up, post the RDMA operation ---
      if (sessionIt != req.headers.end()) {
        const std::string& sid = sessionIt->second;
        if (req.method == "PUT") {
          std::vector<uint8_t> payload;
          if (rdma.completeReadFromClient(sid, payload) != 0) {
            resp.status = 500;
            resp.body = "RDMA PUT complete failed";
            return resp;
          }
          objects[key] = std::move(payload);
          resp.status = 200;
          resp.headers["etag"] = "\"test\"";
          return resp;
        }
        if (req.method == "GET") {
          std::vector<uint8_t> data;
          if (rdma.completeWriteToClient(sid, data) != 0) {
            resp.status = 500;
            resp.body = "RDMA GET complete failed";
            return resp;
          }
          resp.status = 200;
          resp.headers["x-amz-rdma-bytes-transferred"] = std::to_string(
            data.size());
          return resp;
        }
        resp.status = 405;
        resp.body = "method not allowed";
        return resp;
      }

      // --- PREPARE phase: connect QP, stage data, return server token ---
      if (tokenIt == req.headers.end()) {
        resp.status = 400;
        resp.body = "missing x-amz-rdma-token or x-amz-rdma-session";
        return resp;
      }

      if (req.method == "PUT") {
        size_t contentLen = 0;
        auto clIt = req.headers.find("content-length");
        if (clIt != req.headers.end()) {
          contentLen = static_cast<size_t>(
            std::strtoull(clIt->second.c_str(), nullptr, 10));
        }
        std::string replyHeader;
        std::string sid;
        if (rdma.prepareReadFromClient(tokenIt->second, contentLen, replyHeader,
                                       sid) != 0) {
          resp.status = 500;
          resp.body = "RDMA PUT prepare failed";
          return resp;
        }
        resp.status = 200;
        resp.headers["x-amz-rdma-reply"] = replyHeader;
        resp.headers["x-amz-rdma-session"] = sid;
        return resp;
      }

      if (req.method == "GET") {
        auto it = objects.find(key);
        if (it == objects.end()) {
          resp.status = 404;
          resp.body = "not found";
          return resp;
        }
        std::string replyHeader;
        std::string sid;
        if (rdma.prepareWriteToClient(tokenIt->second, it->second, replyHeader,
                                      sid) != 0) {
          resp.status = 500;
          resp.body = "RDMA GET prepare failed";
          return resp;
        }
        resp.status = 200;
        resp.headers["x-amz-rdma-reply"] = replyHeader;
        resp.headers["x-amz-rdma-session"] = sid;
        return resp;
      }

      resp.status = 405;
      resp.body = "method not allowed";
      return resp;
    });

  fprintf(stdout, "hipobj-rdma-test-server listening on port %d\n", port);
  for (;;) {
    server.runOnce(1000);
  }
  return 0;
}
