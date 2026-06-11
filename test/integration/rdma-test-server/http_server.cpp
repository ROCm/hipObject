/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "http_server.hpp"

#include <cerrno>
#include <cstring>
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
  listen(listen_fd_, 8);
}

HttpServer::~HttpServer() {
  if (listen_fd_ >= 0) {
    close(listen_fd_);
  }
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
  send(client, out.data(), out.size(), 0);
  close(client);
  return 1;
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
  oss << "HTTP/1.1 " << resp.status << " OK\r\n";
  oss << "Content-Length: " << resp.body.size() << "\r\n";
  for (const auto& [k, v] : resp.headers) {
    oss << k << ": " << v << "\r\n";
  }
  oss << "\r\n";
  oss << resp.body;
  return oss.str();
}

} // namespace hipobj::test
