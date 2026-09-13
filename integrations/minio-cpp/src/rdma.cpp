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

namespace minio = ::minio;

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

/* Canonical rdma-target fallback: /bucket/key with the sorted query
 * appended. The library normally supplies req->target already built;
 * this covers a null target (defensive) without a second encoder. */
std::string buildObjectTarget(const char* bucket, const char* key,
                              const char* query) {
  std::string t = std::string("/") + (bucket ? bucket : "") + "/" +
                  (key ? key : "");
  if (query && query[0] != '\0') {
    t += "?";
    t += query;
  }
  return t;
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

#ifdef HIPOBJECT_V2_API

struct V2CallbackCtx {
  S3RdmaContext* sctx;
  std::string clientNic; // NIC hint derived from the client token
  /* READY response stash: the HTTP client completes the round trip
   * inside sendReadyRequest, so the FINAL fields are read here when
   * the library calls finishReady. */
  minio::http::Response last_ready;
  bool ready_sent = false;
};

// hipObjOpsV2_t callbacks -------------------------------------------------

/* Builds and signs a control-plane request against the S3 endpoint.
 * The control path lives under /.hipobj-rc/{prepare,ready,cancel} on
 * the same host as the object endpoint; the canonical target (object
 * path + sorted query) travels in a header so the signed path stays
 * the fixed control path. */
minio::http::Response executeControlRequest(
  S3RdmaContext* sctx, const std::string& control_path,
  const std::string& nic, minio::utils::Multimap& extra_headers) {
  minio::utils::UtcTime date = minio::utils::UtcTime::Now();
  minio::creds::Credentials creds = sctx->provider->Fetch();
  minio::utils::Multimap query_params;
  minio::http::Url url;
  const std::string& region = sctx->region;

  if (sctx->url.BuildUrl(url, minio::http::Method::kPost, region,
                         query_params, "", "") ||
      url.host.empty()) {
    minio::http::Response bad;
    bad.status_code = -1;
    return bad;
  }
  url.path = control_path;
  url.query_string.clear();

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

  minio::signer::SignV4S3(minio::http::Method::kPost, control_path, region,
                          sign_headers, query_params, creds.access_key,
                          creds.secret_key, kUnsignedPayload, date);

  minio::http::Request req(minio::http::Method::kPost, url);
  req.headers = sign_headers;
  req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
  req.timeout_secs = kRdmaTimeoutSecs;

  if (!nic.empty()) {
    req.nic_interface = nic;
  }

  return req.Execute();
}

std::string hex32Bridge(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return std::string(buf);
}

std::string hex24Bridge(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%06x", v);
  return std::string(buf);
}

std::string hex64Bridge(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llx",
                static_cast<unsigned long long>(v));
  return std::string(buf);
}

/* The PREPARE reply token arrives as "200:<88-hex>" in
 * X-Amz-Rdma-Reply; strip the status prefix and keep the payload. */
std::string replyTokenPayload(const minio::http::Response& res) {
  std::string v = res.headers.GetFront(kAmzRdmaReplyHdr);
  const std::string prefix = "200:";
  if (v.size() > prefix.size() &&
      v.compare(0, prefix.size(), prefix) == 0) {
    return v.substr(prefix.size());
  }
  return std::string();
}

int v2SendPrepare(void* ctx, const hipObjTransferReqV2_t* req,
                  hipObjPrepareReplyV2_t* out) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
  extra.Add(kAmzRdmaToken, req->token ? req->token : "");
  extra.Add(kAmzRdmaPsnHdr, hex24Bridge(req->clientPsn));
  extra.Add(kAmzRdmaCookieHdr, hex32Bridge(req->cookie));
  extra.Add(kAmzRdmaOpHdr, req->method ? req->method : "GET");
  extra.Add(kAmzRdmaTargetHdr,
            req->target ? req->target
                        : buildObjectTarget(req->bucket, req->key,
                                            req->query));
  extra.Add(kAmzRdmaSizeHdr, std::to_string(req->size));
  if (req->offset != 0) {
    extra.Add(kAmzRdmaOffsetHdr, std::to_string(req->offset));
  }

  minio::http::Response res = executeControlRequest(
    c->sctx, kControlPathPrepare, c->clientNic, extra);
  if (!res.error.empty() || res.status_code <= 0) {
    return -1;
  }

  std::memset(out, 0, sizeof(*out));
  out->httpStatus = res.status_code;
  out->protocolEcho = !res.headers.GetFront(kAmzRdmaProtocol).empty() ? 1 : 0;
  out->unsupportedMarker = (res.status_code == kRdmaReplyNotImplemented) ? 1
                                                                         : 0;

  std::string srv_token = replyTokenPayload(res);
  if (!srv_token.empty()) {
    std::snprintf(out->serverToken, sizeof(out->serverToken), "%s",
                  srv_token.c_str());
  }
  std::string session = res.headers.GetFront(kAmzRdmaSession);
  if (!session.empty()) {
    std::snprintf(out->session, sizeof(out->session), "%s", session.c_str());
  }
  std::string psn = res.headers.GetFront(kAmzRdmaPsnHdr);
  if (!psn.empty()) {
    out->serverPsn =
      static_cast<uint32_t>(std::strtoul(psn.c_str(), nullptr, 16));
  }
  std::string saddr = res.headers.GetFront(kAmzRdmaMrAddrHdr);
  std::string srkey = res.headers.GetFront(kAmzRdmaMrRkeyHdr);
  if (!saddr.empty() && !srkey.empty()) {
    out->stagingAddr = std::strtoull(saddr.c_str(), nullptr, 16);
    out->stagingRkey =
      static_cast<uint32_t>(std::strtoul(srkey.c_str(), nullptr, 16));
    out->stagingPresent =
      (out->stagingAddr != 0 || out->stagingRkey != 0) ? 1 : 0;
  }
  return 0;
}

int v2SendReadyRequest(void* ctx, const hipObjTransferReqV2_t* req) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
  extra.Add(kAmzRdmaSessionHdr, req->session ? req->session : "");
  extra.Add(kAmzRdmaCookieHdr, hex32Bridge(req->cookie));
  extra.Add(kAmzRdmaQpnHdr, hex64Bridge(req->clientQpn));
  extra.Add(kAmzRdmaMrAddrHdr, hex64Bridge(req->clientMrAddr));
  extra.Add(kAmzRdmaMrRkeyHdr, hex32Bridge(req->clientMrRkey));

  /* The READY round trip completes synchronously here (the HTTP
   * client has no half-close split). The library calls this after
   * RTR/RTS so the server can pair immediately; the response is
   * consumed below in v2FinishReady from the stashed context. */
  c->last_ready = executeControlRequest(c->sctx, kControlPathReady,
                                        c->clientNic, extra);
  c->ready_sent = true;
  if (!c->last_ready.error.empty() || c->last_ready.status_code <= 0) {
    return -1;
  }
  return 0;
}

int v2FinishReady(void* ctx, const hipObjTransferReqV2_t* req,
                  hipObjFinalReplyV2_t* out) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);
  if (!c->ready_sent) {
    return -1;
  }
  const minio::http::Response& res = c->last_ready;

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

  std::string echo = res.headers.GetFront(kAmzRdmaCookieHdr);
  if (!echo.empty()) {
    out->cookieEcho =
      static_cast<uint32_t>(std::strtoul(echo.c_str(), nullptr, 16));
    out->cookiePresent = 1;
  }

  std::string etag = res.headers.GetFront("etag");
  if (!etag.empty()) {
    std::string trimmed = minio::utils::Trim(etag, '"');
    std::snprintf(out->etag, sizeof(out->etag), "%s", trimmed.c_str());
    c->sctx->etag = trimmed;
  }

  std::string csum = res.headers.GetFront("x-amz-checksum-crc64nvme");
  if (!csum.empty()) {
    std::snprintf(out->checksumB64, sizeof(out->checksumB64), "%s",
                  csum.c_str());
    c->sctx->checksum = csum;
  }
  return 0;
}

int v2SendCancel(void* ctx, const hipObjTransferReqV2_t* req) {
  auto* c = static_cast<V2CallbackCtx*>(ctx);

  minio::utils::Multimap extra;
  extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
  extra.Add(kAmzRdmaSessionHdr, req->session ? req->session : "");

  executeControlRequest(c->sctx, kControlPathCancel, c->clientNic, extra);
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
  ops.sendReadyRequest = v2SendReadyRequest;
  ops.finishReady = v2FinishReady;
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
  ops.sendReadyRequest = v2SendReadyRequest;
  ops.finishReady = v2FinishReady;
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

#endif /* HIPOBJECT_V2_API */

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
#ifdef HIPOBJECT_V2_API
  // Try the v2 protocol first; fall back to v1 only when the server
  // explicitly signals it does not support hipobj-rc-v2.
  ssize_t ret = rdmaPutV2(ctx, buf, size);
  if (ret != kRdmaNotSupported) {
    return ret;
  }
  ret = -1;
#else
  ssize_t ret = -1;
#endif /* HIPOBJECT_V2_API */
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
#ifdef HIPOBJECT_V2_API
  ssize_t ret = rdmaGetV2(ctx, buf, size);
  if (ret != kRdmaNotSupported) {
    return ret;
  }
  ret = -1;
#else
  ssize_t ret = -1;
#endif /* HIPOBJECT_V2_API */
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
