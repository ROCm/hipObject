/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/rdma.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <hipobj.h>
#include <miniocpp/http.h>
#include <miniocpp/request.h>
#include <miniocpp/signer.h>
#include <miniocpp/utils.h>

namespace hipobj::minio {

namespace {

int parseRdmaReply(const std::string& rdma_reply) {
  if (rdma_reply.empty()) {
    return static_cast<int>(kRdmaNotSupported);
  }
  int httpCode = 0;
  hipObjError_t err = hipObjParseRdmaReply(rdma_reply.c_str(),
                                           rdma_reply.size(), &httpCode);
  if (err.opError != hipObjSuccess) {
    return 0;
  }
  if (httpCode == kRdmaReplyNotImplemented) {
    return static_cast<int>(kRdmaNotSupported);
  }
  return httpCode;
}

std::string clientNicFromToken(const char* token) {
  char nicIp[32];
  hipObjError_t err = hipObjTokenClientNic(token, nicIp, sizeof(nicIp));
  if (err.opError != hipObjSuccess || nicIp[0] == '\0') {
    return {};
  }
  return std::string(nicIp);
}

} // namespace

ssize_t rdmaPut(S3RdmaContext* sctx, const char* token, const void* buf,
                size_t size) {
  char rdma_token[512];
  std::snprintf(rdma_token, sizeof(rdma_token), "%s:%016lx:%016lx", token,
                reinterpret_cast<uintptr_t>(buf),
                static_cast<unsigned long>(size));

  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::http::Url url;
  const std::string& region = sctx->region;

  if (!sctx->uploadId.empty()) {
    query_params.Add("uploadId", sctx->uploadId);
    if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
      return -1;
    }
    query_params.Add("partNumber", std::to_string(sctx->partNumber));
  }

  if (minio::error::Error err =
        sctx->url.BuildUrl(url, minio::http::Method::kPut, region, query_params,
                           sctx->bucket, sctx->object)) {
    return -1;
  }

  std::string host = url.HostHeaderValue();

  minio::utils::Multimap sign_headers;
  sign_headers.Add("Host", host);
  sign_headers.Add("x-amz-date", date.ToAmzDate());
  sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
  sign_headers.Add(kAmzRdmaToken, rdma_token);
  sign_headers.Add("Content-Type", "application/octet-stream");
  sign_headers.Add("Content-Length", "0");

  if (!sctx->checksum.empty()) {
    sign_headers.Add("x-amz-checksum-crc64nvme", sctx->checksum);
  }

  if (!creds.session_token.empty()) {
    sign_headers.Add("X-Amz-Security-Token", creds.session_token);
  }

  minio::signer::SignV4S3(minio::http::Method::kPut, url.path, region,
                          sign_headers, query_params, creds.access_key,
                          creds.secret_key, kUnsignedPayload, date);

  url.query_string = query_params.ToQueryString();

  minio::http::Request req(minio::http::Method::kPut, url);
  req.headers = sign_headers;
  req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
  req.timeout_secs = kRdmaTimeoutSecs;

  std::string client_nic = clientNicFromToken(token);
  if (!client_nic.empty()) {
    req.nic_interface = client_nic;
  }

  minio::http::Response res = req.Execute();
  if (!res.error.empty()) {
    return -1;
  }

  std::string etag = res.headers.GetFront("etag");
  if (res.status_code == 200 && !etag.empty()) {
    sctx->etag = minio::utils::Trim(etag, '"');
    return static_cast<ssize_t>(size);
  }

  int reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
  if (reply_code == static_cast<int>(kRdmaNotSupported)) {
    return kRdmaNotSupported;
  }
  if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyNoContent) {
    return -1;
  }

  std::string resp_checksum = res.headers.GetFront("x-amz-checksum-crc64nvme");
  if (!resp_checksum.empty()) {
    sctx->checksum = resp_checksum;
  }

  sctx->etag = minio::utils::Trim(etag, '"');
  return static_cast<ssize_t>(size);
}

ssize_t rdmaGet(S3RdmaContext* sctx, const char* token, const void* buf,
                size_t size) {
  char rdma_token[512];
  std::snprintf(rdma_token, sizeof(rdma_token), "%s:%016lx:%016lx", token,
                reinterpret_cast<uintptr_t>(buf),
                static_cast<unsigned long>(size));

  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::http::Url url;
  const std::string& region = sctx->region;

  if (minio::error::Error err =
        sctx->url.BuildUrl(url, minio::http::Method::kGet, region, query_params,
                           sctx->bucket, sctx->object)) {
    return -1;
  }

  std::string host = url.HostHeaderValue();

  minio::utils::Multimap sign_headers;
  sign_headers.Add("Host", host);
  sign_headers.Add("x-amz-date", date.ToAmzDate());
  sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
  sign_headers.Add(kAmzRdmaToken, rdma_token);

  if (!creds.session_token.empty()) {
    sign_headers.Add("X-Amz-Security-Token", creds.session_token);
  }

  minio::signer::SignV4S3(minio::http::Method::kGet, url.path, region,
                          sign_headers, query_params, creds.access_key,
                          creds.secret_key, kUnsignedPayload, date);

  minio::http::Request req(minio::http::Method::kGet, url);
  req.headers = sign_headers;
  req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
  req.timeout_secs = kRdmaTimeoutSecs;

  std::string client_nic = clientNicFromToken(token);
  if (!client_nic.empty()) {
    req.nic_interface = client_nic;
  }

  minio::http::Response res = req.Execute();
  if (!res.error.empty()) {
    return -1;
  }

  int reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
  if (reply_code == static_cast<int>(kRdmaNotSupported)) {
    return kRdmaNotSupported;
  }
  if (reply_code != kRdmaReplySuccess &&
      reply_code != kRdmaReplyPartialContent) {
    return -1;
  }

  std::string bytes_hdr = res.headers.GetFront(kAmzRdmaBytesTransferred);
  if (!bytes_hdr.empty()) {
    try {
      long long n = std::stoll(bytes_hdr);
      if (n < 0) {
        return -1;
      }
      return static_cast<ssize_t>(n);
    } catch (const std::exception&) {
      return -1;
    }
  }

  return static_cast<ssize_t>(size);
}

ssize_t rdmaPutWithRetry(S3RdmaContext* ctx, void* buf, size_t size) {
  ssize_t ret = -1;
  for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
    char* token = nullptr;
    hipObjError_t terr = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT,
                                            &token);
    if (terr.opError != hipObjSuccess || token == nullptr) {
      return -1;
    }
    ret = rdmaPut(ctx, token, buf, size);
    hipObjPutRdmaToken(token);
    if (ret > 0 || ret == kRdmaNotSupported) {
      return ret;
    }
  }
  return ret;
}

ssize_t rdmaGetWithRetry(S3RdmaContext* ctx, void* buf, size_t size) {
  ssize_t ret = -1;
  for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
    char* token = nullptr;
    hipObjError_t terr = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET,
                                            &token);
    if (terr.opError != hipObjSuccess || token == nullptr) {
      return -1;
    }
    ret = rdmaGet(ctx, token, buf, size);
    hipObjPutRdmaToken(token);
    if (ret > 0 || ret == kRdmaNotSupported) {
      return ret;
    }
  }
  return ret;
}

} // namespace hipobj::minio
