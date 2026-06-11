/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RC-native S3-over-RDMA integration test server.
 *
 * Accepts GET/PUT with x-amz-rdma-token, performs RC RDMA transfers,
 * and returns x-amz-rdma-reply with an optional peer token for the
 * client-side RC handshake.
 */

#include "http_server.hpp"
#include "rdma_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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
  server.setHandler([&](const hipobj::test::HttpRequest& req)
                        -> hipobj::test::HttpResponse {
    hipobj::test::HttpResponse resp;
    const std::string key = objectKey(req.path);
    auto tokenIt = req.headers.find("x-amz-rdma-token");
    if (tokenIt == req.headers.end()) {
      resp.status = 400;
      resp.body = "missing x-amz-rdma-token";
      return resp;
    }

    if (req.method == "PUT") {
      std::vector<uint8_t> payload;
      std::string replyHeader;
      size_t contentLen = 0;
      auto clIt = req.headers.find("content-length");
      if (clIt != req.headers.end()) {
        contentLen = static_cast<size_t>(std::strtoull(clIt->second.c_str(),
                                                         nullptr, 10));
      }
      if (rdma.rdmaReadFromClient(tokenIt->second, contentLen, payload,
                                  replyHeader) != 0) {
        resp.status = 500;
        resp.body = "RDMA PUT failed";
        return resp;
      }
      objects[key] = std::move(payload);
      resp.status = 200;
      resp.headers["x-amz-rdma-reply"] = replyHeader;
      resp.headers["etag"] = "\"test\"";
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
      if (rdma.rdmaWriteToClient(tokenIt->second, it->second, replyHeader) !=
          0) {
        resp.status = 500;
        resp.body = "RDMA GET failed";
        return resp;
      }
      resp.status = 200;
      resp.headers["x-amz-rdma-reply"] = replyHeader;
      resp.headers["x-amz-rdma-bytes-transferred"] =
          std::to_string(it->second.size());
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
