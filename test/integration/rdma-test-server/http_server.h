/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hipobj::test {

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers; /* lower-case names */
  std::string rawHeaders; /* original header block (credentials) */
  std::string body;
};

struct HttpResponse {
  int status = 500;
  std::map<std::string, std::string> headers;
  std::string body;
  /* Runs exactly once after the response bytes are sent (or the
   * attempt fails). The finalizer is moved out before execution so
   * no path can run it twice; exceptions from the callback are
   * contained. */
  std::function<void(bool sentOk)> afterSend;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
  explicit HttpServer(int port);
  ~HttpServer();

  void setHandler(HttpHandler handler);

  /* v1 single-shot accept loop (unchanged behavior). */
  int runOnce(int timeoutMs);

  /* v2 threaded mode: accepts connections until stop(), handling
   * each on its own thread. Responses are completed with the
   * afterSend finalizer contract. startThreaded() runs the loop
   * on a tracked thread so stop() can join it (production
   * barrier); runThreaded() runs it on the caller's thread. */
  void startThreaded();
  void runThreaded();
  void stop();

  int fd() const {
    return listen_fd_;
  }

private:
  void handleConnection(int client, bool closeFd);

  int listen_fd_ = -1;
  HttpHandler handler_;
  std::atomic<bool> stopping_{false};
  /* The accept loop runs on its own thread when started via
   * startThreaded(); stop() joins it as the production barrier. */
  std::thread acceptLoop_;
  std::mutex workersMtx_;
  /* Detached workers: stop() waits for this count to reach zero
   * after shutting down the registered fds. */
  int liveWorkers_ = 0;
  std::condition_variable workerDoneCv_;
  /* Client fds with a live worker; guarded by workersMtx_. stop()
   * shutdowns these (via dup'd handles) to unblock workers. */
  std::set<int> activeFds_;
};

HttpRequest parseRequest(const std::string& raw);
std::string serializeResponse(const HttpResponse& resp);

} // namespace hipobj::test
