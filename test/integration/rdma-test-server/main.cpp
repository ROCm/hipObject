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

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "http_server.h"
#include "rdma_server.h"
#include "v2_handlers.h"
#include "v2_request.h"
#include "v2_sigv4.h"

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
  bool v2Mode = false;
  bool hangAfterPrepare = false;
  std::string accessKey = "hipobj-test-key";
  std::string secretKey = "hipobj-test-secret";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--v2") {
      v2Mode = true;
    } else if (arg == "--v2-access-key" && i + 1 < argc) {
      accessKey = argv[++i];
    } else if (arg == "--v2-secret-key" && i + 1 < argc) {
      secretKey = argv[++i];
    } else if (arg == "--hang-after-prepare") {
      hangAfterPrepare = true;
    } else {
      port = std::atoi(arg.c_str());
    }
  }

  if (v2Mode) {
    /* v2 reference mode: control protocol on the threaded server.
     * RDMA objects are attached per session by the transport layer;
     * this mode only needs the control plane. */
    hipObj::v2::BuiltinVerifier verifier(accessKey, secretKey, "us-east-1");
    hipObj::v2::MemoryBackend backend;
    hipObj::v2::ServerConfig cfg;
    hipObj::v2::ControlHandlers handlers(&verifier, &backend, cfg);

    hipobj::test::HttpServer server(port);
    server.setHandler(
      [&](const hipobj::test::HttpRequest& req) -> hipobj::test::HttpResponse {
        hipobj::test::HttpResponse resp;
        if (req.path == "/.hipobj-rc/prepare") {
          auto parsed = hipObj::v2::parsePrepareRequest(req.headers,
                                                        req.rawHeaders);
          if (!parsed.has_value()) {
            resp.status = 400;
            return resp;
          }
          auto r = handlers.onPrepare(*parsed);
          resp.status = r.status;
          resp.headers = r.headers;
          if (r.status == 200 && hangAfterPrepare) {
            /* Deliberate stall for the supervisor-reclaim E2E
             * gate; no watchdog inside the process. */
            for (;;) {
              sleep(1);
            }
          }
          if (r.status == 200) {
            /* Publishing -> Prepared when the bytes leave. */
            std::string sid = r.headers.count("X-Amz-Rdma-Session")
                                ? r.headers["X-Amz-Rdma-Session"]
                                : std::string();
            resp.afterSend = [&handlers, sid, cfg](bool ok) {
              if (ok) {
                handlers.table().finishPublishing(sid, cfg.tPrepMs);
              } else {
                handlers.table().toReaping(sid);
              }
            };
          }
          return resp;
        }
        if (req.path == "/.hipobj-rc/ready") {
          auto parsed = hipObj::v2::parseReadyRequest(req.headers,
                                                      req.rawHeaders);
          if (!parsed.has_value()) {
            resp.status = 400;
            return resp;
          }
          auto r = handlers.onReady(*parsed);
          resp.status = r.status;
          resp.headers = r.headers;
          if (r.status == 200 || r.status == 204) {
            std::string sid = parsed->session;
            resp.afterSend = [&handlers, sid](bool) {
              handlers.table().toReaping(sid);
            };
          }
          return resp;
        }
        if (req.path == "/.hipobj-rc/cancel") {
          auto parsed = hipObj::v2::parseCancelRequest(req.headers,
                                                       req.rawHeaders);
          if (!parsed.has_value()) {
            resp.status = 400;
            return resp;
          }
          auto r = handlers.onCancel(*parsed);
          resp.status = r.status;
          return resp;
        }
        /* Object paths with an rdma token: v1 sequences fail
         * structurally in v2 mode - explicit unsupported marker. */
        resp.status = 501;
        resp.headers["X-Amz-Rdma-Protocol-Status"] = "unsupported";
        return resp;
      });
    fprintf(stdout, "hipobj-rdma-test-server v2 listening on port %d\n", port);
    server.runThreaded();
    return 0;
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
          contentLen = static_cast<size_t>(
            std::strtoull(clIt->second.c_str(), nullptr, 10));
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
        resp.headers["x-amz-rdma-bytes-transferred"] = std::to_string(
          it->second.size());
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
