/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/rdma.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

// ---------------------------------------------------------------------------
// v2 callback context — carries the per-transfer minio credentials and
// endpoint so the three hipObjOpsV2_t callbacks can build signed requests.
// ---------------------------------------------------------------------------

struct V2CallbackCtx {
  S3RdmaContext* sctx;
  std::string clientNic; // NIC hint derived from the client token
};

minio::http::Response executeV2Request(
  S3RdmaContext* sctx, minio::http::Method method, const std::string& nic,
  minio::utils::Multimap& extra_headers, const std::string& bucket,
  const std::string& key, const std::string& query_str) {
  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::http::Url url;
  const std::string& region = sctx->region;

  if (minio::error::Error err = sctx->url.BuildUrl(url, method, region,
                                                   query_params, bucket, key)) {
    minio::http::Response bad;
    bad.status_code = -1;
    return bad;
  }

  if (!query_str.empty()) {
    url.query_string = query_str;
  }

  std::string host = url.HostHeaderValue();
  minio::utils::Multimap sign_headers;
  sign_headers.Add("Host", host);
  sign_headers.Add("x-amz-date", date.ToAmzDate());
  sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
  sign_headers.Add("Content-Length", "0");

  for (const auto& [k, vals] : extra_headers.map) {
    for (const auto& v : vals) {
      sign_headers.Add(k, v);
    }
  }

  if (!creds.session_token.empty()) {
    sign_headers.Add("X-Amz-Security-Token", creds.session_token);
  }

  minio::signer::SignV4S3(method, url.path, region, sign_headers, query_params,
                          creds.access_key, creds.secret_key, kUnsignedPayload,
                          date);

  if (!query_str.empty()) {
    url.query_string = query_str;
  } else {
    url.query_string = query_params.ToQueryString();
  }

  minio::http::Request req(method, url);
  req.headers = sign_headers;
  req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
  req.timeout_secs = kRdmaTimeoutSecs;

  if (!nic.empty()) {
    req.nic_interface = nic;
  }

  return req.Execute();
}

// hipObjOpsV2_t callbacks -------------------------------------------------

int v2SendPrepare(void* ctx, const hipObjTransferReqV2_t* req,
                  hipObjPrepareReplyV2_t* out) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);
  const bool isPut = (req->method && req->method[0] == 'P');
  minio::http::Method method = isPut ? minio::http::Method::kPut
                                     : minio::http::Method::kGet;

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaToken, req->token ? req->token : "");
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);

  std::string query = req->query ? req->query : "";
  minio::http::Response res = executeV2Request(c->sctx, method, c->clientNic,
                                               extra,
                                               req->bucket ? req->bucket : "",
                                               req->key ? req->key : "", query);

  if (!res.error.empty() || res.status_code <= 0) {
    return -1;
  }

  std::memset(out, 0, sizeof(*out));
  out->httpStatus = res.status_code;
  out->protocolEcho = !res.headers.GetFront(kAmzRdmaProtocol).empty() ? 1 : 0;
  out->unsupportedMarker = (res.status_code == kRdmaReplyNotImplemented) ? 1
                                                                         : 0;

  std::string srv_token = res.headers.GetFront(kAmzRdmaToken);
  if (!srv_token.empty()) {
    std::snprintf(out->serverToken, sizeof(out->serverToken), "%s",
                  srv_token.c_str());
  }
  std::string session = res.headers.GetFront(kAmzRdmaSession);
  if (!session.empty()) {
    std::snprintf(out->session, sizeof(out->session), "%s", session.c_str());
  }
  return 0;
}

int v2SendReady(void* ctx, const hipObjTransferReqV2_t* req,
                hipObjFinalReplyV2_t* out) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);
  const bool isPut = (req->method && req->method[0] == 'P');
  minio::http::Method method = isPut ? minio::http::Method::kPut
                                     : minio::http::Method::kGet;

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaSession, req->session ? req->session : "");
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);

  std::string query = req->query ? req->query : "";
  minio::http::Response res = executeV2Request(c->sctx, method, c->clientNic,
                                               extra,
                                               req->bucket ? req->bucket : "",
                                               req->key ? req->key : "", query);

  if (!res.error.empty() || res.status_code <= 0) {
    return -1;
  }

  std::memset(out, 0, sizeof(*out));
  out->httpStatus = res.status_code;
  out->protocolEcho = !res.headers.GetFront(kAmzRdmaProtocol).empty() ? 1 : 0;

  std::string bytes_hdr = res.headers.GetFront(kAmzRdmaBytesTransferred);
  if (!bytes_hdr.empty()) {
    try {
      long long n = std::stoll(bytes_hdr);
      out->bytes = (n >= 0) ? static_cast<uint64_t>(n) : 0;
    } catch (const std::exception&) {
    }
  }

  std::string etag = res.headers.GetFront("etag");
  if (!etag.empty()) {
    std::string trimmed = minio::utils::Trim(etag, '"');
    std::snprintf(out->etag, sizeof(out->etag), "%s", trimmed.c_str());
  }

  std::string csum = res.headers.GetFront("x-amz-checksum-crc64nvme");
  if (!csum.empty()) {
    std::snprintf(out->checksumB64, sizeof(out->checksumB64), "%s",
                  csum.c_str());
    c->sctx->checksum = csum;
  }

  if (!etag.empty()) {
    c->sctx->etag = minio::utils::Trim(etag, '"');
  }

  return 0;
}

int v2SendCancel(void* ctx, const hipObjTransferReqV2_t* req) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);
  const bool isPut = (req->method && req->method[0] == 'P');
  minio::http::Method method = isPut ? minio::http::Method::kPut
                                     : minio::http::Method::kGet;

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaSession, req->session ? req->session : "");
  extra.Add(kAmzRdmaCancel, "1");

  std::string query = req->query ? req->query : "";
  executeV2Request(c->sctx, method, c->clientNic, extra,
                   req->bucket ? req->bucket : "", req->key ? req->key : "",
                   query);
  return 0;
}

// v2 entry points ---------------------------------------------------------

ssize_t rdmaPutV2(S3RdmaContext* sctx, void* buf, size_t size) {
  char* token = nullptr;
  hipObjError_t terr = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT,
                                          &token);
  if (terr.opError != hipObjSuccess || !token) {
    return -1;
  }

  V2CallbackCtx cbctx{sctx, clientNicFromToken(token)};
  hipObjOpsV2_t ops{};
  ops.sendPrepare = v2SendPrepare;
  ops.sendReady = v2SendReady;
  ops.sendCancel = v2SendCancel;

  std::string query;
  if (!sctx->uploadId.empty()) {
    if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
      hipObjPutRdmaToken(token);
      return -1;
    }
    query = "uploadId=" + sctx->uploadId +
            "&partNumber=" + std::to_string(sctx->partNumber);
  }

  hipObjError_t err = hipObjPutV2(sctx->bucket.c_str(), sctx->object.c_str(),
                                  buf, static_cast<uint64_t>(size), 0,
                                  query.empty() ? nullptr : query.c_str(), &ops,
                                  &cbctx);
  hipObjPutRdmaToken(token);

  if (err.opError == hipObjNotSupported) {
    return kRdmaNotSupported;
  }
  return (err.opError == hipObjSuccess) ? static_cast<ssize_t>(size) : -1;
}

ssize_t rdmaGetV2(S3RdmaContext* sctx, void* buf, size_t size) {
  char* token = nullptr;
  hipObjError_t terr = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET,
                                          &token);
  if (terr.opError != hipObjSuccess || !token) {
    return -1;
  }

  V2CallbackCtx cbctx{sctx, clientNicFromToken(token)};
  hipObjOpsV2_t ops{};
  ops.sendPrepare = v2SendPrepare;
  ops.sendReady = v2SendReady;
  ops.sendCancel = v2SendCancel;

  hipObjError_t err = hipObjGetV2(sctx->bucket.c_str(), sctx->object.c_str(),
                                  buf, static_cast<uint64_t>(size), 0, nullptr,
                                  &ops, &cbctx);
  hipObjPutRdmaToken(token);

  if (err.opError == hipObjNotSupported) {
    return kRdmaNotSupported;
  }
  return (err.opError == hipObjSuccess) ? static_cast<ssize_t>(size) : -1;
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
  // Try the v2 protocol first; fall back to v1 only when the server
  // explicitly signals it does not support hipobj-rc-v2.
  ssize_t ret = rdmaPutV2(ctx, buf, size);
  if (ret != kRdmaNotSupported) {
    return ret;
  }

  ret = -1;
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
  ssize_t ret = rdmaGetV2(ctx, buf, size);
  if (ret != kRdmaNotSupported) {
    return ret;
  }

  ret = -1;
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
