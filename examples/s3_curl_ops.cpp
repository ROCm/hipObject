/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal libcurl S3 client for hipObject examples.
 */

#include "s3_curl_ops.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <curl/curl.h>

// curl.h defines curl_easy_setopt() and curl_easy_getinfo() as macros that
// expand to themselves, which trips -Wdisabled-macro-expansion. Calls below
// parenthesize the function name to bypass the macro and call the function.

namespace {

struct CurlEasyDeleter {
    void operator()(CURL *curl) const noexcept
    {
        curl_easy_cleanup(curl);
    }
};

struct CurlSlistDeleter {
    void operator()(struct curl_slist *list) const noexcept
    {
        curl_slist_free_all(list);
    }
};

using CurlEasyPtr  = std::unique_ptr<CURL, CurlEasyDeleter>;
using CurlSlistPtr = std::unique_ptr<struct curl_slist, CurlSlistDeleter>;

// curl_slist_append() returns the head of the list, which is a new node
// only when the list was empty, or NULL on failure without freeing the
// list.
bool
appendHeader(CurlSlistPtr &list, const char *header)
{
    struct curl_slist *head = curl_slist_append(list.get(), header);
    if (!head) {
        return false;
    }
    if (!list) {
        list.reset(head);
    }
    return true;
}

struct HeaderState {
    hipObjS3CurlCtx *cfg;
};

size_t
headerCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t      total = size * nitems;
    auto       *state = static_cast<HeaderState *>(userdata);
    std::string line(buffer, total);
    // HTTP header names are case-insensitive.
    for (char &c : line) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c == ':') {
            break;
        }
    }
    const char *prefix = "x-amz-rdma-reply:";
    if (line.rfind(prefix, 0) == 0) {
        std::string val = line.substr(std::strlen(prefix));
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
            val.pop_back();
        }
        size_t start = val.find_first_not_of(' ');
        if (start != std::string::npos) {
            val = val.substr(start);
        }
        std::snprintf(state->cfg->lastReply, sizeof(state->cfg->lastReply), "%s", val.c_str());
    }
    return total;
}

std::string
buildUrl(const hipObjS3CurlCtx *cfg)
{
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

// Returns the HTTP status code of the last transfer, or -1 if libcurl
// cannot report one.
long
responseCode(CURL *curl)
{
    long code = 0;
    if ((curl_easy_getinfo)(curl, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK) {
        return -1;
    }
    return code;
}

} // namespace

extern "C" {

int
hipObjS3CurlSendRequest(void *ctx, const char *token, size_t tokenLen)
{
    auto *cfg = static_cast<hipObjS3CurlCtx *>(ctx);
    if (!cfg || !token) {
        return -1;
    }

    CurlEasyPtr curl(curl_easy_init());
    if (!curl) {
        return -1;
    }

    std::string headerToken = "x-amz-rdma-token: ";
    headerToken.append(token, tokenLen);
    CurlSlistPtr headers;
    if (!appendHeader(headers, headerToken.c_str()) || !appendHeader(headers, "Content-Length: 0")) {
        return -1;
    }

    HeaderState state{cfg};
    cfg->lastReply[0] = '\0';

    (curl_easy_setopt)(curl.get(), CURLOPT_URL, buildUrl(cfg).c_str());
    (curl_easy_setopt)(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    (curl_easy_setopt)(curl.get(), CURLOPT_CUSTOMREQUEST, cfg->isPut ? "PUT" : "GET");
    (curl_easy_setopt)(curl.get(), CURLOPT_HEADERFUNCTION, headerCallback);
    (curl_easy_setopt)(curl.get(), CURLOPT_HEADERDATA, &state);

    CURLcode   rc       = curl_easy_perform(curl.get());
    const long httpCode = rc == CURLE_OK ? responseCode(curl.get()) : -1;
    if (rc != CURLE_OK) {
        return -1;
    }
    if (httpCode < 200 || httpCode >= 300) {
        std::fprintf(stderr, "hipObjS3CurlSendRequest: HTTP %ld\n", httpCode);
        return -1;
    }
    // A 2xx response without an RDMA reply header means the server did not
    // offload the transfer to RDMA. Report the failure instead of treating
    // the missing header as success.
    if (cfg->lastReply[0] == '\0') {
        std::fprintf(stderr, "hipObjS3CurlSendRequest: missing x-amz-rdma-reply header\n");
        return -1;
    }
    return 0;
}

int
hipObjS3CurlRecvReply(void *ctx, char *reply, size_t *replyLen)
{
    auto *cfg = static_cast<hipObjS3CurlCtx *>(ctx);
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
