/* SPDX-License-Identifier: MIT
 */

#include "hipobj_aws_s3_client.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

#include <aws/core/auth/signer/AWSAuthV4Signer.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/http/URI.h>
#include <aws/s3/S3Errors.h>

#include "log.h"

namespace hipobj::aws {

namespace {

    // hipObject has one instance per process, between hipObjInit() and
    // hipObjShutdown(), so they're tracked here across every
    // HipObjAwsS3Client instance, the same way integrations/minio-cpp guards
    // its bridge.
    class HipObjRuntime {
    public:
        static HipObjRuntime &Instance()
        {
            static HipObjRuntime inst;
            return inst;
        }

        hipObjError_t EnsureInit(const Aws::String &endpoint, const Aws::String &region)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Aws::String                 key = endpoint + ":" + region;
            if (initialized_ && key == activeKey_) {
                return {hipObjSuccess, 0};
            }
            if (initialized_) {
                hipObjShutdown();
                initialized_ = false;
            }
            endpointStorage_ = endpoint;
            regionStorage_   = region;
            hipObjConfig_t cfg{};
            cfg.endpoint      = endpointStorage_.c_str();
            cfg.region        = regionStorage_.c_str();
            cfg.gpuDevice     = -1;
            hipObjError_t err = hipObjInit(&cfg);
            if (err.opError == hipObjSuccess) {
                initialized_ = true;
                activeKey_   = key;
            }
            return err;
        }

        // hipObjShutdown(), if a client initialized hipObject. Not called from
        // the destructor, which runs during static destruction: shutting down
        // there would stop the RC-CM listener and release hipObject's libibverbs
        // and librdmacm resources while the process exits, in no set order with
        // the rest of it. hipObject's instance is never destroyed at exit, so a
        // process that doesn't call this leaves the listener running until it
        // exits, which is harmless.
        hipObjError_t Shutdown()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!initialized_) {
                return {hipObjSuccess, 0};
            }
            hipObjError_t err = hipObjShutdown();
            initialized_      = false;
            activeKey_.clear();
            return err;
        }

    private:
        HipObjRuntime() = default;

        std::mutex  mutex_;
        bool        initialized_ = false;
        Aws::String activeKey_;
        Aws::String endpointStorage_;
        Aws::String regionStorage_;
    };

    struct ScopedBufRegistration {
        void *ptr   = nullptr;
        bool  owned = false;
        ScopedBufRegistration(void *p, bool own) : ptr(p), owned(own)
        {
        }
        ~ScopedBufRegistration()
        {
            if (owned && ptr) {
                hipObjBufDeregister(ptr);
            }
        }
    };

    // The path-style URI of bucket/key on endpoint. The key goes in through
    // AddPathSegments(), as the SDK's own S3Client adds it, so the URI keeps
    // its '/' separators and percent-encodes the rest of each segment when
    // it's sent and signed: a key with '?', '#', '%' or a space names that
    // object, rather than becoming URI syntax.
    Aws::Http::URI BuildObjectUri(const Aws::String &endpoint, const Aws::String &bucket,
                                  const Aws::String &key)
    {
        Aws::Http::URI uri(endpoint);
        uri.AddPathSegment(bucket);
        uri.AddPathSegments(key);
        return uri;
    }

    // The bytes [offset, offset + size) of a buffer end at, or 0 when offset
    // is negative or the end doesn't fit in size_t
    size_t RangeEnd(size_t size, off_t offset)
    {
        if (offset < 0 || static_cast<uint64_t>(offset) > std::numeric_limits<size_t>::max() - size) {
            return 0;
        }
        return static_cast<size_t>(offset) + size;
    }

} // namespace

// Bundles what SendControlRequest()/the hipObjOps_t thunks need for one
// PutObjectRdma()/GetObjectRdma() call. Lives here (not in the header) so
// callers never see AWS SDK HTTP internals.
struct HipObjAwsS3Client::RdmaControlCtx {
    const HipObjAwsS3Client *client = nullptr;
    // The caller's request, which the control request is a copy of: one or
    // the other is set
    const Aws::S3::Model::PutObjectRequest *putRequest     = nullptr;
    const Aws::S3::Model::GetObjectRequest *getRequest     = nullptr;
    bool                                    isPut          = false;
    char                                    lastReply[512] = {};
    Aws::String                             etag;
    Aws::String                             versionId;
    bool                                    ok = false;
};

HipObjAwsS3Client::HipObjAwsS3Client(const Aws::S3::S3ClientConfiguration            &clientConfiguration,
                                     std::shared_ptr<Aws::S3::S3EndpointProviderBase> endpointProvider)
    : Aws::S3::S3Client(
          [&clientConfiguration] {
              Aws::S3::S3ClientConfiguration cfg = clientConfiguration;
              cfg.useVirtualAddressing           = false;
              return cfg;
          }(),
          endpointProvider),
      endpointOverride_(clientConfiguration.endpointOverride), region_(clientConfiguration.region)
{
}

HipObjAwsS3Client::HipObjAwsS3Client(const Aws::Auth::AWSCredentials                 &credentials,
                                     const Aws::S3::S3ClientConfiguration            &clientConfiguration,
                                     std::shared_ptr<Aws::S3::S3EndpointProviderBase> endpointProvider)
    : Aws::S3::S3Client(credentials, endpointProvider,
                        [&clientConfiguration] {
                            Aws::S3::S3ClientConfiguration cfg = clientConfiguration;
                            cfg.useVirtualAddressing           = false;
                            return cfg;
                        }()),
      endpointOverride_(clientConfiguration.endpointOverride), region_(clientConfiguration.region)
{
}

hipObjError_t
HipObjAwsS3Client::EnsureHipObjInit() const
{
    return HipObjRuntime::Instance().EnsureInit(endpointOverride_, region_);
}

hipObjError_t
HipObjAwsS3Client::Shutdown()
{
    return HipObjRuntime::Instance().Shutdown();
}

int
HipObjAwsS3Client::SendRequestThunk(void *ctxPtr, const char *token, size_t tokenLen)
{
    auto *ctx = static_cast<RdmaControlCtx *>(ctxPtr);
    return ctx->client->SendControlRequest(*ctx, token, tokenLen);
}

int
HipObjAwsS3Client::RecvReplyThunk(void *ctxPtr, char *reply, size_t *replyLen)
{
    auto *ctx = static_cast<RdmaControlCtx *>(ctxPtr);
    if (!ctx->ok) {
        return -1;
    }
    size_t len = std::strlen(ctx->lastReply);
    if (*replyLen < len) {
        return -1;
    }
    std::memcpy(reply, ctx->lastReply, len);
    *replyLen = len;
    return 0;
}

// Issues one signed HTTP round trip (PUT or GET, zero-length body) that
// carries the hipObject RDMA token in x-amz-rc-rdma-token, mirroring what
// examples/s3_curl_ops.cpp does with plain libcurl -- except this goes
// through the AWS SDK's own SigV4 signer and HTTP client, so it works
// against a real S3-compatible endpoint that requires authentication.
int
HipObjAwsS3Client::SendControlRequest(RdmaControlCtx &ctx, const char *token, size_t tokenLen) const
{
    ctx.ok           = false;
    ctx.lastReply[0] = '\0';

    const Aws::String &bucket = ctx.isPut ? ctx.putRequest->GetBucket() : ctx.getRequest->GetBucket();
    const Aws::String &key    = ctx.isPut ? ctx.putRequest->GetKey() : ctx.getRequest->GetKey();
    Aws::Http::URI     uri    = BuildObjectUri(endpointOverride_, bucket, key);
    if (logEnabled(LogLevel::Debug)) {
        logDebug("{} {}  x-amz-rc-rdma-token ({} bytes)", ctx.isPut ? "PUT" : "GET",
                 uri.GetURIString().c_str(), tokenLen);
    }

    if (ctx.isPut) {
        // The caller's request, metadata and all, carrying the token instead
        // of a body (PutObjectRdma() refused one with a body)
        Aws::S3::Model::PutObjectRequest request = *ctx.putRequest;
        request.SetAdditionalCustomHeaderValue("x-amz-rc-rdma-token", Aws::String(token, tokenLen));

        Aws::Client::HttpResponseOutcome outcome =
            AttemptExhaustively(uri, request, Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER);
        if (!outcome.IsSuccess()) {
            const auto &error = outcome.GetError();
            logError("PUT control request failed: httpStatus={} type={} "
                     "exceptionName=\"{}\" message: {}",
                     static_cast<int>(error.GetResponseCode()), static_cast<int>(error.GetErrorType()),
                     error.GetExceptionName().c_str(), error.GetMessage().c_str());
            return -1;
        }
        const auto &response = outcome.GetResult();
        if (logEnabled(LogLevel::Debug)) {
            logDebug("PUT response httpStatus={} hasRdmaReply={:d}",
                     static_cast<int>(response->GetResponseCode()),
                     response->HasHeader("x-amz-rc-rdma-reply"));
        }
        if (response->HasHeader("etag")) {
            ctx.etag = response->GetHeader("etag");
        }
        if (response->HasHeader("x-amz-version-id")) {
            ctx.versionId = response->GetHeader("x-amz-version-id");
        }
        if (!response->HasHeader("x-amz-rc-rdma-reply")) {
            logError("missing x-amz-rc-rdma-reply header");
            return -1;
        }
        std::snprintf(ctx.lastReply, sizeof(ctx.lastReply), "%s",
                      response->GetHeader("x-amz-rc-rdma-reply").c_str());
    }
    else {
        // The caller's request, with its range, version and conditions
        Aws::S3::Model::GetObjectRequest request = *ctx.getRequest;
        request.SetAdditionalCustomHeaderValue("x-amz-rc-rdma-token", Aws::String(token, tokenLen));

        Aws::Client::HttpResponseOutcome outcome =
            AttemptExhaustively(uri, request, Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER);
        if (!outcome.IsSuccess()) {
            const auto &error = outcome.GetError();
            logError("GET control request failed: httpStatus={} type={} "
                     "exceptionName=\"{}\" message: {}",
                     static_cast<int>(error.GetResponseCode()), static_cast<int>(error.GetErrorType()),
                     error.GetExceptionName().c_str(), error.GetMessage().c_str());
            return -1;
        }
        const auto &response = outcome.GetResult();
        if (logEnabled(LogLevel::Debug)) {
            logDebug("GET response httpStatus={} hasRdmaReply={:d}",
                     static_cast<int>(response->GetResponseCode()),
                     response->HasHeader("x-amz-rc-rdma-reply"));
        }
        if (response->HasHeader("etag")) {
            ctx.etag = response->GetHeader("etag");
        }
        if (!response->HasHeader("x-amz-rc-rdma-reply")) {
            logError("missing x-amz-rc-rdma-reply header");
            return -1;
        }
        std::snprintf(ctx.lastReply, sizeof(ctx.lastReply), "%s",
                      response->GetHeader("x-amz-rc-rdma-reply").c_str());
    }

    ctx.ok = true;
    return 0;
}

PutObjectOutcome
HipObjAwsS3Client::PutObjectRdma(const Aws::S3::Model::PutObjectRequest &request, const void *devPtr,
                                 size_t size, off_t offset) const
{
    if (!request.BucketHasBeenSet() || !request.KeyHasBeenSet()) {
        return PutObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::VALIDATION, "HipObjAwsS3Client",
            "PutObjectRdma requires Bucket and Key to be set", false));
    }
    // The payload goes over RDMA, so the request can't carry or describe
    // an HTTP body of its own
    if (request.GetBody() || request.ContentLengthHasBeenSet() || request.ContentMD5HasBeenSet()) {
        return PutObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::VALIDATION, "HipObjAwsS3Client",
            "PutObjectRdma takes the payload from devPtr: the request can't have "
            "a body, a Content-Length or a Content-MD5",
            false));
    }
    const size_t regSize = RangeEnd(size, offset);
    if (size == 0 || regSize == 0) {
        return PutObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::VALIDATION, "HipObjAwsS3Client",
            "PutObjectRdma needs a nonzero size and an offset that is >= 0 and "
            "doesn't overflow",
            false));
    }

    hipObjError_t initErr = EnsureHipObjInit();
    if (initErr.opError != hipObjSuccess) {
        return PutObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::INTERNAL_FAILURE, "HipObjAwsS3Client",
            Aws::String("hipObjInit failed: ") + hipObjGetErrorString(initErr.opError), false));
    }

    // hipObjPut() moves [offset, offset + size) of the registration at
    // devPtr, so a registration made here covers offset + size bytes. An
    // existing one is used as it is, and hipObjPut() checks the range.
    bool          registeredHere = false;
    hipObjError_t regErr         = hipObjBufRegister(const_cast<void *>(devPtr), regSize);
    if (regErr.opError == hipObjSuccess) {
        registeredHere = true;
    }
    else if (regErr.opError != hipObjBufAlreadyRegistered) {
        return PutObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::INTERNAL_FAILURE, "HipObjAwsS3Client",
            Aws::String("hipObjBufRegister failed: ") + hipObjGetErrorString(regErr.opError), false));
    }
    ScopedBufRegistration regGuard(const_cast<void *>(devPtr), registeredHere);

    RdmaControlCtx ctx;
    ctx.client     = this;
    ctx.putRequest = &request;
    ctx.isPut      = true;

    hipObjOps_t ops;
    ops.sendRequest = &HipObjAwsS3Client::SendRequestThunk;
    ops.recvReply   = &HipObjAwsS3Client::RecvReplyThunk;

    hipObjError_t err = hipObjPut(nullptr, devPtr, size, offset, &ops, &ctx);
    if (err.opError != hipObjSuccess) {
        return PutObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::INTERNAL_FAILURE, "HipObjAwsS3Client",
            Aws::String("hipObjPut failed: ") + hipObjGetErrorString(err.opError), false));
    }

    Aws::S3::Model::PutObjectResult result;
    if (!ctx.etag.empty()) {
        result.SetETag(ctx.etag);
    }
    if (!ctx.versionId.empty()) {
        result.SetVersionId(ctx.versionId);
    }
    return PutObjectOutcome(std::move(result));
}

hipObjError_t
HipObjAwsS3Client::RegisterBuffer(void *devPtr, size_t capacity) const
{
    hipObjError_t initErr = EnsureHipObjInit();
    if (initErr.opError != hipObjSuccess) {
        return initErr;
    }
    return hipObjBufRegister(devPtr, capacity);
}

hipObjError_t
HipObjAwsS3Client::DeregisterBuffer(void *devPtr) const
{
    return hipObjBufDeregister(devPtr);
}

GetObjectOutcome
HipObjAwsS3Client::GetObjectRdma(const Aws::S3::Model::GetObjectRequest &request, void *devPtr, size_t size,
                                 off_t offset) const
{
    if (!request.BucketHasBeenSet() || !request.KeyHasBeenSet()) {
        return GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::VALIDATION, "HipObjAwsS3Client",
            "GetObjectRdma requires Bucket and Key to be set", false));
    }
    const size_t regSize = RangeEnd(size, offset);
    if (size == 0 || regSize == 0) {
        return GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::VALIDATION, "HipObjAwsS3Client",
            "GetObjectRdma needs a nonzero size and an offset that is >= 0 and "
            "doesn't overflow",
            false));
    }

    hipObjError_t initErr = EnsureHipObjInit();
    if (initErr.opError != hipObjSuccess) {
        return GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::INTERNAL_FAILURE, "HipObjAwsS3Client",
            Aws::String("hipObjInit failed: ") + hipObjGetErrorString(initErr.opError), false));
    }

    // As in PutObjectRdma(): a registration made here covers offset + size
    bool          registeredHere = false;
    hipObjError_t regErr         = hipObjBufRegister(devPtr, regSize);
    if (regErr.opError == hipObjSuccess) {
        registeredHere = true;
    }
    else if (regErr.opError != hipObjBufAlreadyRegistered) {
        return GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::INTERNAL_FAILURE, "HipObjAwsS3Client",
            Aws::String("hipObjBufRegister failed: ") + hipObjGetErrorString(regErr.opError), false));
    }
    ScopedBufRegistration regGuard(devPtr, registeredHere);

    RdmaControlCtx ctx;
    ctx.client     = this;
    ctx.getRequest = &request;
    ctx.isPut      = false;

    hipObjOps_t ops;
    ops.sendRequest = &HipObjAwsS3Client::SendRequestThunk;
    ops.recvReply   = &HipObjAwsS3Client::RecvReplyThunk;

    hipObjError_t err = hipObjGet(nullptr, devPtr, size, offset, &ops, &ctx);
    if (err.opError != hipObjSuccess) {
        return GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
            Aws::S3::S3Errors::INTERNAL_FAILURE, "HipObjAwsS3Client",
            Aws::String("hipObjGet failed: ") + hipObjGetErrorString(err.opError), false));
    }

    Aws::S3::Model::GetObjectResult result;
    result.SetContentLength(static_cast<long long>(size));
    if (!ctx.etag.empty()) {
        result.SetETag(ctx.etag);
    }
    return GetObjectOutcome(std::move(result));
}

} // namespace hipobj::aws
