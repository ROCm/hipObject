/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Example: download an S3 object directly into
 *          GPU VRAM via RDMA.
 *
 * Usage:
 *   get-object <size-bytes>              # stub callbacks (no S3)
 *   get-object <size-bytes> --live URL   # libcurl + test server
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <hip/hip_runtime_api.h>

#include "hipobj.h"

#ifdef HIPOBJ_HAVE_CURL
#include <curl/curl.h>

#include "s3_curl_ops.h"
#endif

namespace {

// Frees device memory allocated with hipMalloc()
struct HipFreeDeleter {
    void operator()(void *ptr) const noexcept
    {
        (void)hipFree(ptr);
    }
};

// Shuts hipObject down when it goes out of scope. Create one once
// hipObjInit() has succeeded, before anything that has to be released
// while hipObject is still running, so that's released first.
class HipObjShutdownGuard {
public:
    HipObjShutdownGuard() = default;
    ~HipObjShutdownGuard()
    {
        (void)hipObjShutdown();
    }

    HipObjShutdownGuard(const HipObjShutdownGuard &)            = delete;
    HipObjShutdownGuard &operator=(const HipObjShutdownGuard &) = delete;
};

// Deregisters a buffer from hipObject when it goes out of scope. Create
// one once hipObjBufRegister() has succeeded, after the buffer's owner,
// so the buffer is deregistered before it's freed.
class HipObjBufRegistration {
public:
    explicit HipObjBufRegistration(void *devPtr) : devPtr_(devPtr)
    {
    }
    ~HipObjBufRegistration()
    {
        (void)hipObjBufDeregister(devPtr_);
    }

    HipObjBufRegistration(const HipObjBufRegistration &)            = delete;
    HipObjBufRegistration &operator=(const HipObjBufRegistration &) = delete;

private:
    void *devPtr_;
};

#ifdef HIPOBJ_HAVE_CURL
// Initializes libcurl for as long as it's in scope, when asked to
class CurlGlobal {
public:
    explicit CurlGlobal(bool init) : ok_(init && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
    {
    }
    ~CurlGlobal()
    {
        if (ok_) {
            curl_global_cleanup();
        }
    }

    CurlGlobal(const CurlGlobal &)            = delete;
    CurlGlobal &operator=(const CurlGlobal &) = delete;

    bool ok() const
    {
        return ok_;
    }

private:
    bool ok_;
};
#endif

int
stubSendRequest(void * /*ctx*/, const char * /*token*/, size_t tokenLen)
{
    fprintf(stderr,
            "[get-object] would send RDMA token "
            "(%zu bytes) via S3 GET x-amz-rdma-token\n",
            tokenLen);
    return 0;
}

int
stubRecvReply(void * /*ctx*/, char *reply, size_t *replyLen)
{
    // No real server is contacted in stub mode; reply "501" (not
    // implemented) so the transfer is reported as unsupported rather
    // than silently succeeding.
    const char  *reply501 = "501";
    const size_t len      = strlen(reply501);
    if (*replyLen < len) {
        return -1;
    }
    memcpy(reply, reply501, len);
    *replyLen = len;
    return 0;
}

// Parses a decimal byte count, and returns 0 when the argument isn't one
// or doesn't fit in a size_t
size_t
parseSize(const char *arg)
{
    // strtoull() would skip leading whitespace and accept a sign, and it
    // negates a negative value rather than rejecting it
    if (arg[0] < '0' || arg[0] > '9') {
        return 0;
    }
    errno                        = 0;
    char                    *end = nullptr;
    const unsigned long long n   = std::strtoull(arg, &end, 10);
    if (errno != 0 || *end != '\0' || static_cast<size_t>(n) != n) {
        return 0;
    }
    return static_cast<size_t>(n);
}

} // namespace

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <object-size-bytes> [--live URL [bucket] [object]]\n", argv[0]);
        return 1;
    }

    const size_t objSize = parseSize(argv[1]);
    if (objSize == 0) {
        fprintf(stderr, "Invalid object size: %s\n", argv[1]);
        return 1;
    }

    bool        live     = false;
    const char *endpoint = "http://127.0.0.1:9000";
    const char *bucket   = "test";
    const char *object   = "object";
    if (argc >= 4 && std::strcmp(argv[2], "--live") == 0) {
        live     = true;
        endpoint = argv[3];
        if (argc >= 5) {
            bucket = argv[4];
        }
        if (argc >= 6) {
            object = argv[5];
        }
    }

    hipObjConfig_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.endpoint  = endpoint;
    cfg.region    = "us-east-1";
    cfg.gpuDevice = 0;
    cfg.nicHint   = std::getenv("HIPOBJ_NIC_HINT");

    // The guards below release what they own when main() returns, in the
    // reverse of the order they were created in: the buffer is
    // deregistered, then freed, and then hipObject is shut down.
    hipObjError_t err = hipObjInit(&cfg);
    if (err.opError != hipObjSuccess) {
        fprintf(stderr, "hipObjInit failed: %s\n", hipObjGetErrorString(err.opError));
        return 1;
    }
    const HipObjShutdownGuard shutdownGuard;

    void            *rawDevPtr = nullptr;
    const hipError_t hip_err   = hipMalloc(&rawDevPtr, objSize);
    if (hip_err != hipSuccess) {
        fprintf(stderr, "hipMalloc failed: %d\n", hip_err);
        return 1;
    }
    const std::unique_ptr<void, HipFreeDeleter> devPtr(rawDevPtr);

    err = hipObjBufRegister(devPtr.get(), objSize);
    if (err.opError != hipObjSuccess) {
        fprintf(stderr, "hipObjBufRegister failed: %s\n", hipObjGetErrorString(err.opError));
        return 1;
    }
    const HipObjBufRegistration registration(devPtr.get());

    hipObjOps_t ops;
    void       *opsCtx = nullptr;
#ifdef HIPOBJ_HAVE_CURL
    const CurlGlobal curl(live);
    if (live && !curl.ok()) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }
    hipObjS3CurlCtx curlCtx{};
    if (live) {
        curlCtx.endpoint   = endpoint;
        curlCtx.bucket     = bucket;
        curlCtx.object     = object;
        curlCtx.objectSize = objSize;
        curlCtx.devPtr     = devPtr.get();
        curlCtx.isPut      = 0;
        ops.sendRequest    = hipObjS3CurlSendRequest;
        ops.recvReply      = hipObjS3CurlRecvReply;
        opsCtx             = &curlCtx;
    }
    else
#endif
    {
        (void)live;
        ops.sendRequest = stubSendRequest;
        ops.recvReply   = stubRecvReply;
    }

    err = hipObjGet(nullptr, devPtr.get(), objSize, 0, &ops, opsCtx);
    if (err.opError != hipObjSuccess) {
        fprintf(stderr, "hipObjGet failed: %s\n", hipObjGetErrorString(err.opError));
        return 1;
    }
    fprintf(stdout, "hipObjGet succeeded for %zu bytes\n", objSize);
    return 0;
}
