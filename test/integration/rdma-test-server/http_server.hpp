/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>
#include <map>
#include <string>

namespace hipobj::test {

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status = 500;
  std::map<std::string, std::string> headers;
  std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
  explicit HttpServer(int port);
  ~HttpServer();

  void setHandler(HttpHandler handler);
  int runOnce(int timeoutMs);
  int fd() const { return listen_fd_; }

private:
  int listen_fd_ = -1;
  HttpHandler handler_;
};

HttpRequest parseRequest(const std::string& raw);
std::string serializeResponse(const HttpResponse& resp);

} // namespace hipobj::test
