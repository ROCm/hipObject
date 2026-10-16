/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/client.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <hip/hip_runtime_api.h>

#include <hipobj.h>
#include <miniocpp/args.h>
#include <miniocpp/error.h>
#include <miniocpp/http.h>

#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj_minio/context.h"
#include "hipobj_minio/rdma.h"

namespace hipobj::minio {

namespace {

    /* Monotonic, process-wide, and deliberately not per-Client: a caller that
     * wants to know whether the RDMA path was taken does not want to thread a
     * Client reference through its assertions. constinit makes sure they're
     * initialized at compile time, before anything can count. */
    constinit std::atomic<uint64_t> g_rdmaPuts{0};
    constinit std::atomic<uint64_t> g_rdmaGets{0};
    constinit std::atomic<uint64_t> g_httpPuts{0};
    constinit std::atomic<uint64_t> g_httpGets{0};
    constinit std::atomic<uint64_t> g_rdmaBytes{0};
    constinit std::atomic<uint64_t> g_httpBytes{0};

    void CountRdmaPut(size_t n)
    {
        g_rdmaPuts.fetch_add(1, std::memory_order_relaxed);
        g_rdmaBytes.fetch_add(n, std::memory_order_relaxed);
    }
    void CountRdmaGet(size_t n)
    {
        g_rdmaGets.fetch_add(1, std::memory_order_relaxed);
        g_rdmaBytes.fetch_add(n, std::memory_order_relaxed);
    }
    void CountHttpPut(size_t n)
    {
        g_httpPuts.fetch_add(1, std::memory_order_relaxed);
        g_httpBytes.fetch_add(n, std::memory_order_relaxed);
    }
    void CountHttpGet(size_t n)
    {
        g_httpGets.fetch_add(1, std::memory_order_relaxed);
        g_httpBytes.fetch_add(n, std::memory_order_relaxed);
    }

    bool IsDevicePointer(const void *ptr)
    {
        hipPointerAttribute_t attr{};
        if (hipPointerGetAttributes(&attr, ptr) != hipSuccess) {
            // An unregistered host pointer is an error here, not an answer.
            (void)hipGetLastError();
            return false;
        }
        return attr.type == hipMemoryTypeDevice;
    }

    // Why an RDMA transfer didn't move the data, for messages
    const char *DescribeFailure(const RdmaResult &result)
    {
        if (result.status == RdmaStatus::NotSupported) {
            return "not supported";
        }
        if (result.status == RdmaStatus::Failed) {
            return "error";
        }
        return "no bytes transferred";
    }

    class HipObjRuntime {
    public:
        static HipObjRuntime &Instance()
        {
            // Holds only a mutex and strings, so destroying it at exit is safe
            HIPOBJ_WARN_NO_EXIT_DTOR_OFF
            static HipObjRuntime inst;
            HIPOBJ_WARN_NO_EXIT_DTOR_ON
            return inst;
        }

        hipObjError_t EnsureInit(const ::minio::s3::BaseUrl &base_url)
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            const std::string            key = base_url.host + ":" + base_url.region;
            if (initialized_ && key == active_key_) {
                return {.opError = hipObjSuccess, .hipError = 0};
            }
            if (initialized_) {
                hipObjShutdown();
                initialized_ = false;
            }
            hipObjConfig_t cfg{};
            endpoint_storage_ = (base_url.https ? "https://" : "http://") + base_url.host;
            cfg.endpoint      = endpoint_storage_.c_str();
            cfg.region        = base_url.region.c_str();
            cfg.gpuDevice     = -1;
            // Without a GPU there is no topology to walk, so hipObjInit can only find
            // a NIC if it is named explicitly. Same variable the examples read.
            if (const char *hint = std::getenv("HIPOBJ_NIC_HINT"); hint != nullptr && *hint != '\0') {
                nic_hint_storage_ = hint;
                cfg.nicHint       = nic_hint_storage_.c_str();
            }
            hipObjError_t err = hipObjInit(&cfg);
            if (err.opError == hipObjSuccess) {
                initialized_ = true;
                active_key_  = key;
            }
            return err;
        }

        [[nodiscard]] bool IsReady() const
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            return initialized_;
        }

        ~HipObjRuntime() = default;

        HipObjRuntime(const HipObjRuntime &)            = delete;
        HipObjRuntime &operator=(const HipObjRuntime &) = delete;
        HipObjRuntime(HipObjRuntime &&)                 = delete;
        HipObjRuntime &operator=(HipObjRuntime &&)      = delete;

    private:
        HipObjRuntime() = default;

        mutable std::mutex mutex_{};
        bool               initialized_ = false;
        std::string        active_key_{};
        std::string        endpoint_storage_{};
        std::string        nic_hint_storage_{};
    };

    // Deregisters a buffer from hipObjBufRegister(). Owning the buffer's
    // registration this way releases it on every path, including when a
    // transfer throws.
    struct BufDeregisterer {
        void operator()(void *ptr) const noexcept
        {
            (void)hipObjBufDeregister(ptr);
        }
    };

    using BufRegistrationPtr = std::unique_ptr<void, BufDeregisterer>;

    /* The response to a transfer that RDMA completed. Client::PutObject and
     * Client::GetObject return other objects on their other paths, which
     * rules out the named return value optimization for a local there, so
     * they return this one as a prvalue instead. */
    template <typename ResponseT> ResponseT RdmaResponse(std::string etag)
    {
        ResponseT resp;
        resp.etag = std::move(etag);
        return resp;
    }

    /* An HTTP GET of the whole object into args.buf, which holds size bytes.
     * The object is accumulated in a string and copied to args.buf only if
     * the GET succeeds. */
    ::minio::s3::GetObjectResponse HttpGetIntoBuffer(::minio::s3::Client              &client,
                                                     const ::minio::s3::GetObjectArgs &args, size_t size)
    {
        std::string received;
        received.reserve(size);
        ::minio::s3::GetObjectArgs http_args = args;
        http_args.datafunc                   = [&](const ::minio::http::DataFunctionArgs &chunk) -> bool {
            received.append(chunk.datachunk);
            return true;
        };
        http_args.buf                       = nullptr;
        http_args.size                      = std::nullopt;
        ::minio::s3::GetObjectResponse resp = client.GetObject(http_args);
        if (resp) {
            const std::span<char> dst = hipObj::spanFromPointer(args.buf, size);
            std::ranges::copy(std::string_view(received).substr(0, dst.size()), dst.begin());
        }
        CountHttpGet(size);
        return resp;
    }

} // namespace

TransferStats
TransferStatsSnapshot()
{
    TransferStats s;
    s.rdmaPuts  = g_rdmaPuts.load(std::memory_order_relaxed);
    s.rdmaGets  = g_rdmaGets.load(std::memory_order_relaxed);
    s.httpPuts  = g_httpPuts.load(std::memory_order_relaxed);
    s.httpGets  = g_httpGets.load(std::memory_order_relaxed);
    s.rdmaBytes = g_rdmaBytes.load(std::memory_order_relaxed);
    s.httpBytes = g_httpBytes.load(std::memory_order_relaxed);
    return s;
}

void
TransferStatsReset()
{
    g_rdmaPuts.store(0, std::memory_order_relaxed);
    g_rdmaGets.store(0, std::memory_order_relaxed);
    g_httpPuts.store(0, std::memory_order_relaxed);
    g_httpGets.store(0, std::memory_order_relaxed);
    g_rdmaBytes.store(0, std::memory_order_relaxed);
    g_httpBytes.store(0, std::memory_order_relaxed);
}

std::string
TransferStatsLine(const TransferStats &stats)
{
    std::ostringstream line;
    // A stream formats numbers with the global locale, which the application
    // may have set to one that groups digits ("131,072")
    line.imbue(std::locale::classic());
    line << "hipobj-stats: rdma_put=" << stats.rdmaPuts << " rdma_get=" << stats.rdmaGets
         << " http_put=" << stats.httpPuts << " http_get=" << stats.httpGets
         << " rdma_bytes=" << stats.rdmaBytes << " http_bytes=" << stats.httpBytes;
    return line.str();
}

Client::Client(::minio::s3::BaseUrl base_url, ::minio::creds::Provider *provider)
    : base_url_(base_url), provider_(provider), s3_client_(base_url, provider)
{
}

bool
Client::RdmaAvailable()
{
    return HipObjRuntime::Instance().IsReady();
}

::minio::s3::PutObjectResponse
Client::PutObject(::minio::s3::PutObjectArgs args)
{
    if (const ::minio::error::Error err = args.Validate()) {
        return ::minio::s3::PutObjectResponse(err);
    }

    if (args.buf == nullptr) {
        return s3_client_.PutObject(args);
    }
    // Validate() requires a size with buf, but that's out of sight here
    if (!args.size) {
        return ::minio::s3::PutObjectResponse(::minio::error::Error("size must be set when buf is set"));
    }

    const size_t size = *args.size;
    // minio-cpp's object_size is a long, which the HTTP paths below set, and
    // long is only 32 bits on some platforms
    if (!std::in_range<long>(size)) {
        return ::minio::s3::PutObjectResponse(::minio::error::Error("object size is too large"));
    }

    const hipObjError_t init_err = HipObjRuntime::Instance().EnsureInit(base_url_);
    if (init_err.opError != hipObjSuccess) {
        // RDMA not available (no NIC or no GPU topology) — fall back to HTTP PUT.
        // Copy the buffer into a std::stringstream so minio-cpp can seek in it.
        const char                *src = static_cast<const char *>(args.buf);
        const std::string          content(src, size);
        std::stringstream          ss(content, std::ios_base::in | std::ios_base::binary);
        ::minio::s3::PutObjectArgs http_args = args;
        http_args.stream                     = &ss;
        http_args.buf                        = nullptr;
        http_args.size                       = std::nullopt;
        http_args.object_size                = static_cast<long>(size);
        http_args.part_size                  = 0;
        CountHttpPut(size);
        return s3_client_.PutObject(http_args);
    }

    const hipObjError_t reg_err = hipObjBufRegister(args.buf, size);
    if (reg_err.opError != hipObjSuccess && reg_err.opError != hipObjBufAlreadyRegistered) {
        return ::minio::s3::PutObjectResponse(::minio::error::Error(
            "hipObjBufRegister failed: " + std::string(hipObjGetErrorString(reg_err.opError))));
    }
    // Only a registration made here is undone here
    const BufRegistrationPtr registration(reg_err.opError == hipObjSuccess ? args.buf : nullptr);

    std::string region;
    if (const ::minio::s3::GetRegionResponse resp = s3_client_.GetRegion(args.bucket, args.region)) {
        region = resp.region;
    }
    else {
        return ::minio::s3::PutObjectResponse(resp);
    }

    S3RdmaContext put_ctx{
        .provider = provider_,
        .bucket   = args.bucket,
        .object   = args.object,
        .url      = base_url_,
        .region   = region,
    };

    const RdmaResult ret = rdmaPutWithRetry(&put_ctx, args.buf, size);
    if (ret.status == RdmaStatus::Ok && ret.bytes > 0) {
        CountRdmaPut(ret.bytes);
        return RdmaResponse<::minio::s3::PutObjectResponse>(std::move(put_ctx.etag));
    }

    // The fallback below streams the buffer from the host, so it cannot be
    // taken for device memory, and it turns a GPU-direct transfer into an
    // ordinary one. A caller that handed us a device pointer asked for RDMA.
    if (IsDevicePointer(args.buf)) {
        return ::minio::s3::PutObjectResponse(
            ::minio::error::Error("RDMA PUT failed (" + std::string(DescribeFailure(ret)) +
                                  ") and the payload is device memory; no HTTP fallback is possible"));
    }
    std::cerr << "hipobj: RDMA PUT failed (" << DescribeFailure(ret) << "); falling back to HTTP\n";

    ::minio::s3::PutObjectArgs http_args = args;
    std::stringstream          ss(std::ios_base::in | std::ios_base::out | std::ios_base::binary);
    ss.rdbuf()->pubsetbuf(args.buf, static_cast<std::streamsize>(size));
    http_args.stream      = &ss;
    http_args.buf         = nullptr;
    http_args.size        = std::nullopt;
    http_args.object_size = static_cast<long>(size);
    // Validate() clamped part_size down to the object size on the way in, and
    // anything under 5MiB fails the next Validate. Let it be recomputed.
    http_args.part_size = 0;
    CountHttpPut(size);
    return s3_client_.PutObject(http_args);
}

::minio::s3::GetObjectResponse
Client::GetObject(::minio::s3::GetObjectArgs args)
{
    if (const ::minio::error::Error err = args.Validate()) {
        return ::minio::s3::GetObjectResponse(err);
    }

    if (args.buf == nullptr) {
        return s3_client_.GetObject(args);
    }
    // Validate() requires a size with buf, but that's out of sight here
    if (!args.size) {
        return ::minio::s3::GetObjectResponse(::minio::error::Error("size must be set when buf is set"));
    }

    const size_t size = *args.size;

    const hipObjError_t init_err = HipObjRuntime::Instance().EnsureInit(base_url_);
    if (init_err.opError != hipObjSuccess) {
        // RDMA not available — fall back to HTTP GET.
        return HttpGetIntoBuffer(s3_client_, args, size);
    }

    const hipObjError_t reg_err = hipObjBufRegister(args.buf, size);
    if (reg_err.opError != hipObjSuccess && reg_err.opError != hipObjBufAlreadyRegistered) {
        return ::minio::s3::GetObjectResponse(::minio::error::Error(
            "hipObjBufRegister failed: " + std::string(hipObjGetErrorString(reg_err.opError))));
    }
    // Only a registration made here is undone here
    const BufRegistrationPtr registration(reg_err.opError == hipObjSuccess ? args.buf : nullptr);

    std::string region;
    if (const ::minio::s3::GetRegionResponse resp = s3_client_.GetRegion(args.bucket, args.region)) {
        region = resp.region;
    }
    else {
        return ::minio::s3::GetObjectResponse(resp);
    }

    S3RdmaContext get_ctx{
        .provider = provider_,
        .bucket   = args.bucket,
        .object   = args.object,
        .url      = base_url_,
        .region   = region,
    };

    const RdmaResult ret = rdmaGetWithRetry(&get_ctx, args.buf, size);
    if (ret.status == RdmaStatus::Ok && ret.bytes > 0) {
        CountRdmaGet(ret.bytes);
        return RdmaResponse<::minio::s3::GetObjectResponse>(std::move(get_ctx.etag));
    }

    // As in PutObject: the fallback memcpys into args.buf from the host.
    if (IsDevicePointer(args.buf)) {
        return ::minio::s3::GetObjectResponse(
            ::minio::error::Error("RDMA GET failed (" + std::string(DescribeFailure(ret)) +
                                  ") and the destination is device memory; no HTTP fallback is possible"));
    }
    std::cerr << "hipobj: RDMA GET failed (" << DescribeFailure(ret) << "); falling back to HTTP\n";

    ::minio::s3::GetObjectArgs http_args = args;
    const std::span<char>      dst       = hipObj::spanFromPointer(args.buf, size);
    size_t                     offset    = 0;
    http_args.datafunc                   = [&](const ::minio::http::DataFunctionArgs &chunk) -> bool {
        /* Anything past size is dropped */
        const std::span<char>  rest = dst.subspan(offset);
        const std::string_view data = std::string_view(chunk.datachunk).substr(0, rest.size());
        std::ranges::copy(data, rest.begin());
        offset += data.size();
        return true;
    };
    http_args.buf  = nullptr;
    http_args.size = std::nullopt;
    CountHttpGet(size);
    return s3_client_.GetObject(http_args);
}

} // namespace hipobj::minio
