/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/rdma.h"

#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include <hipobj.h>
#include <miniocpp/credentials.h>
#include <miniocpp/error.h>
#include <miniocpp/http.h>
#include <miniocpp/providers.h>
#include <miniocpp/request.h>
#include <miniocpp/signer.h>
#include <miniocpp/utils.h>

#include "bytes-transferred.h"
#include "hipobj-span.h"
#include "hipobj_minio/context.h"

namespace hipobj::minio {

/* The enclosing namespace name shadows the minio-cpp root namespace
 * for qualified lookups; rebind it, and keep the using-directive for
 * unqualified names the rest of this file already uses. */
namespace minio = ::minio;
using namespace ::minio;

namespace {

    // A failed transfer used to surface as a bare -1 with no indication of which
    // of the dozen failure points produced it. Set HIPOBJ_RDMA_DEBUG=1 to get one.
    bool rdmaDebugEnabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("HIPOBJ_RDMA_DEBUG");
            return v != nullptr && v[0] != '\0' && v[0] != '0';
        }();
        return on;
    }

    // Prints args, streamed one after another, as a line on stderr when
    // HIPOBJ_RDMA_DEBUG is set. The line is built first and written with one
    // fprintf(), which POSIX makes atomic with respect to other stdio calls
    // on the stream, so lines from concurrent transfers don't interleave. One
    // std::cerr insertion has no such guarantee.
    template <typename... Args> void rdmaTrace(const Args &...args)
    {
        if (!rdmaDebugEnabled()) {
            return;
        }
        std::ostringstream line;
        line << "hipobj: rdma: ";
        (line << ... << args);
        line << '\n';
        const std::string text = line.str();
        (void)std::fprintf(stderr, "%s", text.c_str());
    }

    // The HTTP status in an x-amz-rdma-reply value, or std::nullopt if it's
    // malformed. A server that doesn't support RDMA leaves the header out,
    // which gives kRdmaReplyNotImplemented, as an explicit 501 does.
    std::optional<int> parseRdmaReply(const std::string &rdma_reply)
    {
        if (rdma_reply.empty()) {
            return kRdmaReplyNotImplemented;
        }
        int           httpCode = 0;
        hipObjError_t err      = hipObjParseRdmaReply(rdma_reply.c_str(), rdma_reply.size(), &httpCode);
        if (err.opError != hipObjSuccess) {
            return std::nullopt;
        }
        return httpCode;
    }

    std::string clientNicFromToken(const char *token)
    {
        char          nicIp[32];
        hipObjError_t err = hipObjTokenClientNic(token, nicIp, sizeof(nicIp));
        if (err.opError != hipObjSuccess || nicIp[0] == '\0') {
            return {};
        }
        return std::string(nicIp);
    }

    // A response header's value, or std::nullopt if the response doesn't
    // have the header. GetFront() alone returns "" for both.
    std::optional<std::string> headerValue(const minio::utils::Multimap &headers, const char *name)
    {
        if (!headers.Contains(name)) {
            return std::nullopt;
        }
        return headers.GetFront(name);
    }

    // Releases a token from hipObjGetRdmaToken(). Owning the token this way
    // releases it on every path, including when building or sending the
    // request throws.
    struct RdmaTokenDeleter {
        void operator()(char *token) const noexcept
        {
            (void)hipObjPutRdmaToken(token);
        }
    };

    using RdmaTokenPtr = std::unique_ptr<char, RdmaTokenDeleter>;

    // ---------------------------------------------------------------------------
    // v2 callback context — carries the per-transfer minio credentials and
    // endpoint so the three hipObjOpsV2_t callbacks can build signed requests.
    // ---------------------------------------------------------------------------

#ifdef HIPOBJECT_V2_API

    // Copies src into the C string buffer dst, cut to fit
    void copyCString(std::span<char> dst, const std::string &src)
    {
        std::snprintf(dst.data(), dst.size(), "%s", src.c_str());
    }

    struct V2CallbackCtx {
        S3RdmaContext         *sctx;
        std::string            clientNic; // NIC hint derived from the client token
        minio::http::Response  lastReady;
        bool                   readySent = false;
    };

    std::string hexFixed(uint64_t value, int width)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%0*llx", width, static_cast<unsigned long long>(value));
        return std::string(buf);
    }

    // The PREPARE reply token arrives as "200:<88-hex>" in X-Amz-Rdma-Reply.
    std::string replyTokenPayload(const minio::http::Response &res)
    {
        std::string        value  = res.headers.GetFront(kAmzRdmaReply);
        const std::string  prefix = "200:";
        if (value.size() > prefix.size() && value.compare(0, prefix.size(), prefix) == 0) {
            return value.substr(prefix.size());
        }
        return {};
    }

    // POST to a fixed /.hipobj-rc/* path. The object identity travels in a
    // header, so the signed path stays the control path.
    minio::http::Response executeControlRequest(S3RdmaContext *sctx, const std::string &controlPath,
                                                 const std::string &nic, minio::utils::Multimap &extraHeaders)
    {
        minio::utils::UtcTime     date   = minio::utils::UtcTime::Now();
        minio::creds::Credentials creds  = sctx->provider->Fetch();
        minio::utils::Multimap    queryParams;
        minio::http::Url          url;
        const std::string        &region = sctx->region;

        if (minio::error::Error err =
                sctx->url.BuildUrl(url, minio::http::Method::kPost, region, queryParams, "", "")) {
            minio::http::Response bad;
            bad.status_code = -1;
            return bad;
        }
        url.path = controlPath;
        url.query_string.clear();

        std::string            host = url.HostHeaderValue();
        minio::utils::Multimap signHeaders;
        signHeaders.Add("Host", host);
        signHeaders.Add("x-amz-date", date.ToAmzDate());
        signHeaders.Add("x-amz-content-sha256", kUnsignedPayload);
        signHeaders.Add("Content-Length", "0");

        signHeaders.AddAll(extraHeaders);
        if (!creds.session_token.empty()) {
            signHeaders.Add("X-Amz-Security-Token", creds.session_token);
        }

        minio::signer::SignV4S3(minio::http::Method::kPost, controlPath, region, signHeaders, queryParams,
                                creds.access_key, creds.secret_key, kUnsignedPayload, date);

        minio::http::Request req(minio::http::Method::kPost, url);
        req.headers              = std::move(signHeaders);
        req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
        req.timeout_secs         = kRdmaTimeoutSecs;
        if (!nic.empty()) {
            req.nic_interface = nic;
        }
        return req.Execute();
    }

    minio::http::Response executeV2Request(S3RdmaContext *sctx, minio::http::Method method,
                                           const std::string &nic, minio::utils::Multimap &extra_headers,
                                           const std::string &bucket, const std::string &key,
                                           const std::string &query_str)
    {
        minio::utils::UtcTime     date  = minio::utils::UtcTime::Now();
        minio::creds::Credentials creds = sctx->provider->Fetch();
        minio::utils::Multimap    query_params;
        minio::http::Url          url;
        const std::string        &region = sctx->region;

        if (minio::error::Error err = sctx->url.BuildUrl(url, method, region, query_params, bucket, key)) {
            minio::http::Response bad;
            bad.status_code = -1;
            return bad;
        }

        if (!query_str.empty()) {
            url.query_string = query_str;
        }

        std::string            host = url.HostHeaderValue();
        minio::utils::Multimap sign_headers;
        sign_headers.Add("Host", host);
        sign_headers.Add("x-amz-date", date.ToAmzDate());
        sign_headers.Add("x-amz-content-sha256", kUnsignedPayload);
        sign_headers.Add("Content-Length", "0");

        sign_headers.AddAll(extra_headers);

        if (!creds.session_token.empty()) {
            sign_headers.Add("X-Amz-Security-Token", creds.session_token);
        }

        minio::signer::SignV4S3(method, url.path, region, sign_headers, query_params, creds.access_key,
                                creds.secret_key, kUnsignedPayload, date);

        if (!query_str.empty()) {
            url.query_string = query_str;
        }
        else {
            url.query_string = query_params.ToQueryString();
        }

        minio::http::Request req(method, url);
        req.headers              = std::move(sign_headers);
        req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
        req.timeout_secs         = kRdmaTimeoutSecs;

        if (!nic.empty()) {
            req.nic_interface = nic;
        }

        return req.Execute();
    }

    // hipObjOpsV2_t callbacks -------------------------------------------------

    int v2SendPrepare(void *ctx, const hipObjTransferReqV2_t *req, hipObjPrepareReplyV2_t *out)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);

        minio::utils::Multimap extra;
        extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
        extra.Add(kAmzRdmaToken, req->token ? req->token : "");
        extra.Add(kAmzRdmaPsn, hexFixed(req->clientPsn, 6));
        extra.Add(kAmzRdmaCookie, hexFixed(req->cookie, 8));
        extra.Add(kAmzRdmaOp, req->method ? req->method : "GET");
        extra.Add(kAmzRdmaTarget, req->target ? req->target : "");
        extra.Add(kAmzRdmaSize, std::to_string(req->size));
        if (req->offset != 0) {
            extra.Add(kAmzRdmaOffset, std::to_string(req->offset));
        }

        minio::http::Response res = executeControlRequest(c->sctx, kControlPathPrepare, c->clientNic, extra);
        if (!res.error.empty() || res.status_code <= 0) {
            rdmaTrace("v2 prepare: http failed (status=", res.status_code, " error='", res.error, "')");
            return -1;
        }

        hipObj::zeroObject(*out);
        out->httpStatus        = res.status_code;
        out->protocolEcho      = !res.headers.GetFront(kAmzRdmaProtocol).empty() ? 1 : 0;
        out->unsupportedMarker = (res.status_code == kRdmaReplyNotImplemented) ? 1 : 0;

        std::string srvToken = replyTokenPayload(res);
        if (!srvToken.empty()) {
            copyCString(out->serverToken, srvToken);
        }
        std::string session = res.headers.GetFront(kAmzRdmaSession);
        if (!session.empty()) {
            copyCString(out->session, session);
        }
        std::string psn = res.headers.GetFront(kAmzRdmaPsn);
        if (!psn.empty()) {
            out->serverPsn = static_cast<uint32_t>(std::strtoul(psn.c_str(), nullptr, 16));
        }
        std::string stagingAddr = res.headers.GetFront(kAmzRdmaMrAddr);
        std::string stagingRkey = res.headers.GetFront(kAmzRdmaMrRkey);
        if (!stagingAddr.empty() && !stagingRkey.empty()) {
            out->stagingAddr    = std::strtoull(stagingAddr.c_str(), nullptr, 16);
            out->stagingRkey    = static_cast<uint32_t>(std::strtoul(stagingRkey.c_str(), nullptr, 16));
            out->stagingPresent = (out->stagingAddr != 0 || out->stagingRkey != 0) ? 1 : 0;
        }
        return 0;
    }

    int v2SendReadyRequest(void *ctx, const hipObjTransferReqV2_t *req)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);

        minio::utils::Multimap extra;
        extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
        extra.Add(kAmzRdmaSession, req->session ? req->session : "");
        extra.Add(kAmzRdmaCookie, hexFixed(req->cookie, 8));
        extra.Add(kAmzRdmaQpn, hexFixed(req->clientQpn, 8));
        extra.Add(kAmzRdmaMrAddr, hexFixed(req->clientMrAddr, 16));
        extra.Add(kAmzRdmaMrRkey, hexFixed(req->clientMrRkey, 8));

        // The HTTP client completes the round trip here. finishReady reads
        // the stashed FINAL response.
        c->lastReady = executeControlRequest(c->sctx, kControlPathReady, c->clientNic, extra);
        c->readySent = true;
        if (!c->lastReady.error.empty() || c->lastReady.status_code <= 0) {
            rdmaTrace("v2 ready: http failed (status=", c->lastReady.status_code, " error='", c->lastReady.error,
                      "')");
            return -1;
        }
        return 0;
    }

    int v2FinishReady(void *ctx, const hipObjTransferReqV2_t *req, hipObjFinalReplyV2_t *out)
    {
        (void)req;
        auto *c = static_cast<V2CallbackCtx *>(ctx);
        if (!c->readySent) {
            return -1;
        }
        const minio::http::Response &res = c->lastReady;

        hipObj::zeroObject(*out);
        out->httpStatus   = res.status_code;
        out->protocolEcho = !res.headers.GetFront(kAmzRdmaProtocol).empty() ? 1 : 0;

        const auto bytes = parseBytesTransferred(headerValue(res.headers, kAmzRdmaBytesTransferred), 0);
        if (bytes) {
            out->bytes = *bytes;
        }

        std::string echo = res.headers.GetFront(kAmzRdmaCookie);
        if (!echo.empty()) {
            out->cookieEcho    = static_cast<uint32_t>(std::strtoul(echo.c_str(), nullptr, 16));
            out->cookiePresent = 1;
        }

        std::string etag = res.headers.GetFront("etag");
        if (!etag.empty()) {
            std::string trimmed = minio::utils::Trim(etag, '"');
            copyCString(out->etag, trimmed);
            c->sctx->etag = trimmed;
        }

        std::string csum = res.headers.GetFront("x-amz-checksum-crc64nvme");
        if (!csum.empty()) {
            copyCString(out->checksumB64, csum);
            c->sctx->checksum = csum;
        }
        return 0;
    }

    int v2SendCancel(void *ctx, const hipObjTransferReqV2_t *req)
    {
        auto *c = static_cast<V2CallbackCtx *>(ctx);

        minio::utils::Multimap extra;
        extra.Add(kAmzRdmaProtocol, kAmzRdmaProtocolV2);
        extra.Add(kAmzRdmaSession, req->session ? req->session : "");
        extra.Add(kAmzRdmaCancel, "1");
        executeControlRequest(c->sctx, kControlPathCancel, c->clientNic, extra);
        return 0;
    }

    // v2 entry points ---------------------------------------------------------

    RdmaResult rdmaPutV2(S3RdmaContext *sctx, void *buf, size_t size)
    {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("v2 put: token: ", hipObjGetErrorString(terr.opError));
            return {RdmaStatus::Failed};
        }

        V2CallbackCtx cbctx{sctx, clientNicFromToken(token.get()), {}};
        hipObjOpsV2_t ops{};
        ops.sendPrepare       = v2SendPrepare;
        ops.sendReadyRequest  = v2SendReadyRequest;
        ops.finishReady       = v2FinishReady;
        ops.sendCancel        = v2SendCancel;

        std::string query;
        if (!sctx->uploadId.empty()) {
            if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
                rdmaTrace("v2 put: part number ", sctx->partNumber, " out of range");
                return {RdmaStatus::Failed};
            }
            // hipObjPutV2() takes a canonical query: percent-encoded, with
            // the keys sorted. Upload IDs can contain reserved characters
            // such as '+' and '/', and Multimap encodes and sorts.
            minio::utils::Multimap query_params;
            query_params.Add("uploadId", sctx->uploadId);
            query_params.Add("partNumber", std::to_string(sctx->partNumber));
            query = query_params.ToQueryString();
        }

        hipObjError_t err = hipObjPutV2(sctx->bucket.c_str(), sctx->object.c_str(), buf, size, 0,
                                        query.empty() ? nullptr : query.c_str(), &ops, &cbctx);

        if (err.opError == hipObjNotSupported) {
            rdmaTrace("v2 put: unsupported (client v2 is a stub); falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError == hipObjSizeTooLarge) {
            // v2 caps a transfer at 2^31-1 bytes; v1 can move it
            rdmaTrace("v2 put: ", size, " bytes is too large for v2; falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError != hipObjSuccess) {
            rdmaTrace("v2 put: hipObjPutV2: ", hipObjGetErrorString(err.opError));
            return {RdmaStatus::Failed};
        }
        return {RdmaStatus::Ok, size};
    }

    RdmaResult rdmaGetV2(S3RdmaContext *sctx, void *buf, size_t size)
    {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("v2 get: token: ", hipObjGetErrorString(terr.opError));
            return {RdmaStatus::Failed};
        }

        V2CallbackCtx cbctx{sctx, clientNicFromToken(token.get()), {}};
        hipObjOpsV2_t ops{};
        ops.sendPrepare       = v2SendPrepare;
        ops.sendReadyRequest  = v2SendReadyRequest;
        ops.finishReady       = v2FinishReady;
        ops.sendCancel        = v2SendCancel;

        hipObjError_t err =
            hipObjGetV2(sctx->bucket.c_str(), sctx->object.c_str(), buf, size, 0, nullptr, &ops, &cbctx);

        if (err.opError == hipObjNotSupported) {
            rdmaTrace("v2 get: unsupported (client v2 is a stub); falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError == hipObjSizeTooLarge) {
            // v2 caps a transfer at 2^31-1 bytes; v1 can move it
            rdmaTrace("v2 get: ", size, " bytes is too large for v2; falling back to v1");
            return {RdmaStatus::NotSupported};
        }
        if (err.opError != hipObjSuccess) {
            rdmaTrace("v2 get: hipObjGetV2: ", hipObjGetErrorString(err.opError));
            return {RdmaStatus::Failed};
        }
        return {RdmaStatus::Ok, size};
    }

#endif /* HIPOBJECT_V2_API */

} // namespace

RdmaResult
rdmaPut(S3RdmaContext *sctx, const char *token, size_t size)
{
    // The header carries one fixed-width token and nothing else; a
    // ":<addr>:<len>" suffix would override the address the token already
    // advertises, and on the staging path the caller's pointer is not that
    // address. See docs/interop.rst.
    const std::string rdma_token(token ? token : "");

    minio::utils::UtcTime     date  = minio::utils::UtcTime::Now();
    minio::creds::Credentials creds = sctx->provider->Fetch();
    minio::utils::Multimap    query_params;
    minio::http::Url          url;
    const std::string        &region = sctx->region;

    if (!sctx->uploadId.empty()) {
        query_params.Add("uploadId", sctx->uploadId);
        if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
            return {RdmaStatus::Failed};
        }
        query_params.Add("partNumber", std::to_string(sctx->partNumber));
    }

    if (minio::error::Error err = sctx->url.BuildUrl(url, minio::http::Method::kPut, region, query_params,
                                                     sctx->bucket, sctx->object)) {
        return {RdmaStatus::Failed};
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

    minio::signer::SignV4S3(minio::http::Method::kPut, url.path, region, sign_headers, query_params,
                            creds.access_key, creds.secret_key, kUnsignedPayload, date);

    url.query_string = query_params.ToQueryString();

    minio::http::Request req(minio::http::Method::kPut, url);
    req.headers              = std::move(sign_headers);
    req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
    req.timeout_secs         = kRdmaTimeoutSecs;

    std::string client_nic = clientNicFromToken(token);
    if (!client_nic.empty()) {
        req.nic_interface = client_nic;
    }

    minio::http::Response res = req.Execute();
    if (!res.error.empty()) {
        rdmaTrace("v1 put: http failed (status=", res.status_code, " error='", res.error, "')");
        return {RdmaStatus::Failed};
    }

    std::string etag = res.headers.GetFront("etag");
    if (res.status_code == 200 && !etag.empty()) {
        sctx->etag = minio::utils::Trim(etag, '"');
        return {RdmaStatus::Ok, size};
    }

    const auto reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
    if (reply_code == kRdmaReplyNotImplemented) {
        return {RdmaStatus::NotSupported};
    }
    if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyNoContent) {
        rdmaTrace("v1 put: rdma reply ", reply_code.value_or(0), " (http status ", res.status_code, ")");
        return {RdmaStatus::Failed};
    }

    std::string resp_checksum = res.headers.GetFront("x-amz-checksum-crc64nvme");
    if (!resp_checksum.empty()) {
        sctx->checksum = resp_checksum;
    }

    sctx->etag = minio::utils::Trim(etag, '"');
    return {RdmaStatus::Ok, size};
}

RdmaResult
rdmaGet(S3RdmaContext *sctx, const char *token, size_t size)
{
    // The header carries one fixed-width token and nothing else; a
    // ":<addr>:<len>" suffix would override the address the token already
    // advertises, and on the staging path the caller's pointer is not that
    // address. See docs/interop.rst.
    const std::string rdma_token(token ? token : "");

    minio::utils::UtcTime     date  = minio::utils::UtcTime::Now();
    minio::creds::Credentials creds = sctx->provider->Fetch();
    minio::utils::Multimap    query_params;
    minio::http::Url          url;
    const std::string        &region = sctx->region;

    if (minio::error::Error err = sctx->url.BuildUrl(url, minio::http::Method::kGet, region, query_params,
                                                     sctx->bucket, sctx->object)) {
        return {RdmaStatus::Failed};
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

    minio::signer::SignV4S3(minio::http::Method::kGet, url.path, region, sign_headers, query_params,
                            creds.access_key, creds.secret_key, kUnsignedPayload, date);

    minio::http::Request req(minio::http::Method::kGet, url);
    req.headers              = std::move(sign_headers);
    req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
    req.timeout_secs         = kRdmaTimeoutSecs;

    std::string client_nic = clientNicFromToken(token);
    if (!client_nic.empty()) {
        req.nic_interface = client_nic;
    }

    minio::http::Response res = req.Execute();
    if (!res.error.empty()) {
        rdmaTrace("v1 get: http failed (status=", res.status_code, " error='", res.error, "')");
        return {RdmaStatus::Failed};
    }

    const auto reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
    if (reply_code == kRdmaReplyNotImplemented) {
        return {RdmaStatus::NotSupported};
    }
    if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyPartialContent) {
        rdmaTrace("v1 get: rdma reply ", reply_code.value_or(0), " (http status ", res.status_code, ")");
        return {RdmaStatus::Failed};
    }

    // Without the header, the whole buffer was transferred. The server can't
    // have transferred more than that.
    const auto bytes = parseBytesTransferred(headerValue(res.headers, kAmzRdmaBytesTransferred), size);
    if (!bytes || *bytes > size) {
        rdmaTrace("v1 get: malformed or out-of-range ", kAmzRdmaBytesTransferred, " header");
        return {RdmaStatus::Failed};
    }
    return {RdmaStatus::Ok, static_cast<size_t>(*bytes)};
}

RdmaResult
rdmaPutWithRetry(S3RdmaContext *ctx, void *buf, size_t size)
{
#ifdef HIPOBJECT_V2_API
    // Try the v2 protocol first; fall back to v1 only when the server
    // explicitly signals it does not support hipobj-rc-v2.
    const RdmaResult v2 = rdmaPutV2(ctx, buf, size);
    if (v2.status != RdmaStatus::NotSupported) {
        return v2;
    }
#endif /* HIPOBJECT_V2_API */
    RdmaResult ret{RdmaStatus::Failed};
    for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("v1 put: token: ", hipObjGetErrorString(terr.opError));
            return {RdmaStatus::Failed};
        }
        // A buffer the NIC cannot reach directly is registered as a host
        // staging buffer, so the bytes have to be there before the server
        // reads it.
        hipObjError_t serr = hipObjBufSync(buf, size, 0, HIPOBJ_SYNC_TO_HOST);
        if (serr.opError != hipObjSuccess) {
            rdmaTrace("v1 put: stage to host: ", hipObjGetErrorString(serr.opError));
            return {RdmaStatus::Failed};
        }
        ret = rdmaPut(ctx, token.get(), size);
        if ((ret.status == RdmaStatus::Ok && ret.bytes > 0) || ret.status == RdmaStatus::NotSupported) {
            return ret;
        }
    }
    return ret;
}

RdmaResult
rdmaGetWithRetry(S3RdmaContext *ctx, void *buf, size_t size)
{
#ifdef HIPOBJECT_V2_API
    const RdmaResult v2 = rdmaGetV2(ctx, buf, size);
    if (v2.status != RdmaStatus::NotSupported) {
        return v2;
    }
#endif /* HIPOBJECT_V2_API */
    RdmaResult ret{RdmaStatus::Failed};
    for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("v1 get: token: ", hipObjGetErrorString(terr.opError));
            return {RdmaStatus::Failed};
        }
        ret = rdmaGet(ctx, token.get(), size);
        if (ret.status == RdmaStatus::Ok && ret.bytes > 0) {
            hipObjError_t serr = hipObjBufSync(buf, size, 0, HIPOBJ_SYNC_TO_DEVICE);
            if (serr.opError != hipObjSuccess) {
                rdmaTrace("v1 get: stage to device: ", hipObjGetErrorString(serr.opError));
                return {RdmaStatus::Failed};
            }
            return ret;
        }
        if (ret.status == RdmaStatus::NotSupported) {
            return ret;
        }
    }
    return ret;
}

} // namespace hipobj::minio
