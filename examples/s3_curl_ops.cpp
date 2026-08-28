/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal libcurl S3 client for hipObject examples.
 */

#include "s3_curl_ops.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <curl/curl.h>

namespace {

// Prefix written by sendRdmaReady() to distinguish a READY payload from an
// RDMA token in the ops->sendRequest callback.
const char kSessionPrefix[] = "session:";
const size_t kSessionPrefixLen = sizeof(kSessionPrefix) - 1;

struct HeaderState {
  hipObjS3CurlCtx* cfg;
  bool isReady; // true when processing a READY (not PREPARE) request
};

size_t headerCallback(char* buffer, size_t size, size_t nitems,
                      void* userdata) {
  size_t total = size * nitems;
  auto* state = static_cast<HeaderState*>(userdata);
  std::string line(buffer, total);
  // HTTP header names are case-insensitive.
  for (char& c : line) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c == ':') {
      break;
    }
  }

  auto extractValue = [&](const char* prefix) -> std::string {
    if (line.rfind(prefix, 0) != 0) {
      return {};
    }
    std::string val = line.substr(std::strlen(prefix));
    while (!val.empty() &&
           (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
      val.pop_back();
    }
    size_t start = val.find_first_not_of(' ');
    return (start != std::string::npos) ? val.substr(start) : std::string{};
  };

  std::string rdmaReply = extractValue("x-amz-rdma-reply:");
  if (!rdmaReply.empty()) {
    std::snprintf(state->cfg->lastReply, sizeof(state->cfg->lastReply), "%s",
                  rdmaReply.c_str());
  }
  return total;
}

std::string buildUrl(const hipObjS3CurlCtx* cfg) {
  std::string url = cfg->endpoint ? cfg->endpoint : "http://127.0.0.1:9000";
  if (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  url += '/';
  url += cfg->bucket ? cfg->bucket : "test";
  url += '/';
  url += cfg->object ? cfg->object : "object";
  return url;
}

} // namespace

extern "C" {

int hipObjS3CurlSendRequest(void* ctx, const char* token, size_t tokenLen) {
  auto* cfg = static_cast<hipObjS3CurlCtx*>(ctx);
  if (!cfg || !token) {
    return -1;
  }

  const bool isReady = (tokenLen >= kSessionPrefixLen &&
                        std::memcmp(token, kSessionPrefix, kSessionPrefixLen) == 0);

  CURL* curl = curl_easy_init();
  if (!curl) {
    return -1;
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Length: 0");

  if (isReady) {
    // READY phase: send session ID in x-amz-rdma-session, no RDMA token.
    const char* sessionId = token + kSessionPrefixLen;
    size_t sessionLen = tokenLen - kSessionPrefixLen;
    char sessionHeader[256];
    std::snprintf(sessionHeader, sizeof(sessionHeader),
                  "x-amz-rdma-session: %.*s",
                  static_cast<int>(sessionLen), sessionId);
    headers = curl_slist_append(headers, sessionHeader);
  } else {
    // PREPARE phase: send RDMA token in x-amz-rdma-token.
    char rdmaHeader[512];
    std::snprintf(rdmaHeader, sizeof(rdmaHeader), "%.*s:%016lx:%016lx",
                  static_cast<int>(tokenLen), token,
                  reinterpret_cast<uintptr_t>(cfg->devPtr),
                  static_cast<unsigned long>(cfg->objectSize));
    std::string headerToken = std::string("x-amz-rdma-token: ") + rdmaHeader;
    headers = curl_slist_append(headers, headerToken.c_str());
  }

  HeaderState state{cfg, isReady};
  cfg->lastReply[0] = '\0';

  curl_easy_setopt(curl, CURLOPT_URL, buildUrl(cfg).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cfg->isPut ? "PUT" : "GET");
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);

  CURLcode rc = curl_easy_perform(curl);
  long httpCode = 0;
  if (rc == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    return -1;
  }
  if (httpCode < 200 || httpCode >= 300) {
    std::fprintf(stderr, "hipObjS3CurlSendRequest: HTTP %ld\n", httpCode);
    return -1;
  }

  if (!isReady) {
    // PREPARE: the server must echo an x-amz-rdma-reply carrying the server
    // token + session ID. A missing header means RDMA was not offloaded.
    if (cfg->lastReply[0] == '\0') {
      std::fprintf(stderr,
                   "hipObjS3CurlSendRequest: missing x-amz-rdma-reply header\n");
      return -1;
    }
  } else {
    // READY: synthesise a "200" reply so recvReply can decode it as success.
    std::snprintf(cfg->lastReply, sizeof(cfg->lastReply), "200");
  }
  return 0;
}

int hipObjS3CurlRecvReply(void* ctx, char* reply, size_t* replyLen) {
  auto* cfg = static_cast<hipObjS3CurlCtx*>(ctx);
  if (!cfg || !reply || !replyLen) {
    return -1;
  }
  size_t len = std::strlen(cfg->lastReply);
  if (*replyLen < len) {
    return -1;
  }
  std::memcpy(reply, cfg->lastReply, len);
  *replyLen = len;
  return 0;
}

} // extern "C"
