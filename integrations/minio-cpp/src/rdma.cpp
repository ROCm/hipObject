/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/rdma.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
#include "hipobj_minio/context.h"

namespace hipobj::minio {

namespace {

    using namespace std::string_view_literals;

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
    //
    // Pass text as std::string_view literals ("..."sv), not plain string
    // literals: a plain one arrives as a reference to a char array, which
    // streaming it would decay to a pointer.
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
        int                 httpCode = 0;
        const hipObjError_t err      = hipObjParseRdmaReply(rdma_reply.c_str(), rdma_reply.size(), &httpCode);
        if (err.opError != hipObjSuccess) {
            return std::nullopt;
        }
        return httpCode;
    }

    std::string clientNicFromToken(const char *token)
    {
        std::array<char, 32> nicIp{};
        const hipObjError_t  err = hipObjTokenClientNic(token, nicIp.data(), nicIp.size());
        if (err.opError != hipObjSuccess) {
            return {};
        }
        // Up to the first NUL, which gives "" when no address is encoded
        return std::string(nicIp.begin(), std::ranges::find(nicIp, '\0'));
    }

    // A response header's value, or std::nullopt if the response doesn't
    // have the header. GetFront() alone returns "" for both.
    std::optional<std::string> headerValue(const ::minio::utils::Multimap &headers, std::string_view name)
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

} // namespace

RdmaResult
rdmaPut(S3RdmaContext *sctx, const char *token, size_t size)
{
    // The header carries one fixed-width token and nothing else; a
    // ":<addr>:<len>" suffix would override the address the token already
    // advertises, and on the staging path the caller's pointer is not that
    // address. See docs/interop.rst.
    const std::string rdma_token(token != nullptr ? token : "");

    const ::minio::utils::UtcTime     date  = ::minio::utils::UtcTime::Now();
    const ::minio::creds::Credentials creds = sctx->provider->Fetch();
    ::minio::utils::Multimap          query_params;
    ::minio::http::Url                url;
    const std::string                &region = sctx->region;

    if (!sctx->uploadId.empty()) {
        query_params.Add("uploadId", sctx->uploadId);
        if (sctx->partNumber == 0 || sctx->partNumber > 10000) {
            return {.status = RdmaStatus::Failed};
        }
        query_params.Add("partNumber", std::to_string(sctx->partNumber));
    }

    if (const ::minio::error::Error err = sctx->url.BuildUrl(url, ::minio::http::Method::kPut, region,
                                                             query_params, sctx->bucket, sctx->object)) {
        return {.status = RdmaStatus::Failed};
    }

    const std::string host = url.HostHeaderValue();

    ::minio::utils::Multimap sign_headers;
    sign_headers.Add("Host", host);
    sign_headers.Add("x-amz-date", date.ToAmzDate());
    sign_headers.Add("x-amz-content-sha256", std::string(kUnsignedPayload));
    sign_headers.Add(std::string(kAmzRdmaToken), rdma_token);
    sign_headers.Add("Content-Type", "application/octet-stream");
    sign_headers.Add("Content-Length", "0");

    if (!sctx->checksum.empty()) {
        sign_headers.Add("x-amz-checksum-crc64nvme", sctx->checksum);
    }

    if (!creds.session_token.empty()) {
        sign_headers.Add("X-Amz-Security-Token", creds.session_token);
    }

    ::minio::signer::SignV4S3(::minio::http::Method::kPut, url.path, region, sign_headers, query_params,
                              creds.access_key, creds.secret_key, std::string(kUnsignedPayload), date);

    url.query_string = query_params.ToQueryString();

    ::minio::http::Request req(::minio::http::Method::kPut, url);
    req.headers              = std::move(sign_headers);
    req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
    req.timeout_secs         = kRdmaTimeoutSecs;

    const std::string client_nic = clientNicFromToken(token);
    if (!client_nic.empty()) {
        req.nic_interface = client_nic;
    }

    const ::minio::http::Response res = req.Execute();
    if (!res.error.empty()) {
        rdmaTrace("put: http failed (status="sv, res.status_code, " error='"sv, res.error, "')"sv);
        return {.status = RdmaStatus::Failed};
    }

    const std::string etag = res.headers.GetFront("etag");
    if (res.status_code == 200 && !etag.empty()) {
        sctx->etag = ::minio::utils::Trim(etag, '"');
        return {.status = RdmaStatus::Ok, .bytes = size};
    }

    const auto reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
    if (reply_code == kRdmaReplyNotImplemented) {
        return {.status = RdmaStatus::NotSupported};
    }
    if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyNoContent) {
        rdmaTrace("put: rdma reply "sv, reply_code.value_or(0), " (http status "sv, res.status_code, ")"sv);
        return {.status = RdmaStatus::Failed};
    }

    const std::string resp_checksum = res.headers.GetFront("x-amz-checksum-crc64nvme");
    if (!resp_checksum.empty()) {
        sctx->checksum = resp_checksum;
    }

    sctx->etag = ::minio::utils::Trim(etag, '"');
    return {.status = RdmaStatus::Ok, .bytes = size};
}

RdmaResult
rdmaGet(S3RdmaContext *sctx, const char *token, size_t size)
{
    // The header carries one fixed-width token and nothing else; a
    // ":<addr>:<len>" suffix would override the address the token already
    // advertises, and on the staging path the caller's pointer is not that
    // address. See docs/interop.rst.
    const std::string rdma_token(token != nullptr ? token : "");

    const ::minio::utils::UtcTime     date  = ::minio::utils::UtcTime::Now();
    const ::minio::creds::Credentials creds = sctx->provider->Fetch();
    const ::minio::utils::Multimap    query_params;
    ::minio::http::Url                url;
    const std::string                &region = sctx->region;

    if (const ::minio::error::Error err = sctx->url.BuildUrl(url, ::minio::http::Method::kGet, region,
                                                             query_params, sctx->bucket, sctx->object)) {
        return {.status = RdmaStatus::Failed};
    }

    const std::string host = url.HostHeaderValue();

    ::minio::utils::Multimap sign_headers;
    sign_headers.Add("Host", host);
    sign_headers.Add("x-amz-date", date.ToAmzDate());
    sign_headers.Add("x-amz-content-sha256", std::string(kUnsignedPayload));
    sign_headers.Add(std::string(kAmzRdmaToken), rdma_token);

    if (!creds.session_token.empty()) {
        sign_headers.Add("X-Amz-Security-Token", creds.session_token);
    }

    ::minio::signer::SignV4S3(::minio::http::Method::kGet, url.path, region, sign_headers, query_params,
                              creds.access_key, creds.secret_key, std::string(kUnsignedPayload), date);

    ::minio::http::Request req(::minio::http::Method::kGet, url);
    req.headers              = std::move(sign_headers);
    req.connect_timeout_secs = kRdmaConnectTimeoutSecs;
    req.timeout_secs         = kRdmaTimeoutSecs;

    const std::string client_nic = clientNicFromToken(token);
    if (!client_nic.empty()) {
        req.nic_interface = client_nic;
    }

    const ::minio::http::Response res = req.Execute();
    if (!res.error.empty()) {
        rdmaTrace("get: http failed (status="sv, res.status_code, " error='"sv, res.error, "')"sv);
        return {.status = RdmaStatus::Failed};
    }

    const auto reply_code = parseRdmaReply(res.headers.GetFront(kAmzRdmaReply));
    if (reply_code == kRdmaReplyNotImplemented) {
        return {.status = RdmaStatus::NotSupported};
    }
    if (reply_code != kRdmaReplySuccess && reply_code != kRdmaReplyPartialContent) {
        rdmaTrace("get: rdma reply "sv, reply_code.value_or(0), " (http status "sv, res.status_code, ")"sv);
        return {.status = RdmaStatus::Failed};
    }

    // Without the header, the whole buffer was transferred. The server can't
    // have transferred more than that.
    const auto bytes = parseBytesTransferred(headerValue(res.headers, kAmzRdmaBytesTransferred), size);
    if (!bytes || *bytes > size) {
        rdmaTrace("get: malformed or out-of-range "sv, kAmzRdmaBytesTransferred, " header"sv);
        return {.status = RdmaStatus::Failed};
    }
    return {.status = RdmaStatus::Ok, .bytes = static_cast<size_t>(*bytes)};
}

RdmaResult
rdmaPutWithRetry(S3RdmaContext *ctx, void *buf, size_t size)
{
    RdmaResult ret{.status = RdmaStatus::Failed};
    for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_PUT, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("put: token: "sv, hipObjGetErrorString(terr.opError));
            return {.status = RdmaStatus::Failed};
        }
        // A buffer the NIC cannot reach directly is registered as a host
        // staging buffer, so the bytes have to be there before the server
        // reads it.
        const hipObjError_t serr = hipObjBufSync(buf, size, 0, HIPOBJ_SYNC_TO_HOST);
        if (serr.opError != hipObjSuccess) {
            rdmaTrace("put: stage to host: "sv, hipObjGetErrorString(serr.opError));
            return {.status = RdmaStatus::Failed};
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
    RdmaResult ret{.status = RdmaStatus::Failed};
    for (int attempt = 0; attempt < kRdmaMaxAttempts; ++attempt) {
        char               *rawToken = nullptr;
        const hipObjError_t terr     = hipObjGetRdmaToken(buf, size, HIPOBJ_RDMA_OP_GET, &rawToken);
        const RdmaTokenPtr  token(rawToken);
        if (terr.opError != hipObjSuccess || !token) {
            rdmaTrace("get: token: "sv, hipObjGetErrorString(terr.opError));
            return {.status = RdmaStatus::Failed};
        }
        ret = rdmaGet(ctx, token.get(), size);
        if (ret.status == RdmaStatus::Ok && ret.bytes > 0) {
            const hipObjError_t serr = hipObjBufSync(buf, size, 0, HIPOBJ_SYNC_TO_DEVICE);
            if (serr.opError != hipObjSuccess) {
                rdmaTrace("get: stage to device: "sv, hipObjGetErrorString(serr.opError));
                return {.status = RdmaStatus::Failed};
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
