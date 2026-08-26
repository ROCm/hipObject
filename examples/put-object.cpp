/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Example: upload GPU VRAM to an S3 object
 *          directly via RDMA.
 *
 * Usage:
 *   put-object <size-bytes>              # stub callbacks (no S3)
 *   put-object <size-bytes> --live URL   # libcurl + test server
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <hip/hip_runtime.h>

#include "hipobj.h"

#if defined(HIPOBJ_HAVE_CURL)
#include <curl/curl.h>

#include "s3_curl_ops.h"
#endif

static int stubSendRequest(void* ctx, const char* token, size_t tokenLen) {
  (void)ctx;
  fprintf(stderr,
          "[put-object] would send RDMA token "
          "(%zu bytes) via S3 PUT x-amz-rdma-token\n",
          tokenLen);
  return 0;
}

static int stubRecvReply(void* ctx, char* reply, size_t* replyLen) {
  (void)ctx;
  // No real server is contacted in stub mode; reply "501" (not
  // implemented) so the transfer is reported as unsupported rather
  // than silently succeeding.
  const char* reply501 = "501";
  size_t len = strlen(reply501);
  if (*replyLen < len) {
    return -1;
  }
  memcpy(reply, reply501, len);
  *replyLen = len;
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr,
            "Usage: %s <object-size-bytes> [--live URL [bucket] [object]]\n",
            argv[0]);
    return 1;
  }

  size_t objSize = static_cast<size_t>(atol(argv[1]));
  if (objSize == 0) {
    fprintf(stderr, "Invalid object size: %s\n", argv[1]);
    return 1;
  }

  bool live = false;
  const char* endpoint = "http://127.0.0.1:9000";
  const char* bucket = "test";
  const char* object = "object";
  if (argc >= 4 && std::strcmp(argv[2], "--live") == 0) {
    live = true;
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
  cfg.endpoint = endpoint;
  cfg.region = "us-east-1";
  cfg.gpuDevice = 0;

  hipObjError_t err = hipObjInit(&cfg);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr, "hipObjInit failed: %s\n",
            hipObjGetErrorString(err.opError));
    return 1;
  }

  void* devPtr = nullptr;
  hipError_t hip_err = hipMalloc(&devPtr, objSize);
  if (hip_err != hipSuccess) {
    fprintf(stderr, "hipMalloc failed: %d\n", hip_err);
    hipObjShutdown();
    return 1;
  }

  err = hipObjBufRegister(devPtr, objSize);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr, "hipObjBufRegister failed: %s\n",
            hipObjGetErrorString(err.opError));
    (void)hipFree(devPtr);
    hipObjShutdown();
    return 1;
  }

  hipObjOps_t ops;
  void* opsCtx = nullptr;
#if defined(HIPOBJ_HAVE_CURL)
  hipObjS3CurlCtx curlCtx{};
  if (live) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curlCtx.endpoint = endpoint;
    curlCtx.bucket = bucket;
    curlCtx.object = object;
    curlCtx.objectSize = objSize;
    curlCtx.devPtr = devPtr;
    curlCtx.isPut = 1;
    ops.sendRequest = hipObjS3CurlSendRequest;
    ops.recvReply = hipObjS3CurlRecvReply;
    opsCtx = &curlCtx;
  } else
#endif
  {
    (void)live;
    ops.sendRequest = stubSendRequest;
    ops.recvReply = stubRecvReply;
  }

  int exitCode = 0;
  err = hipObjPut(nullptr, devPtr, objSize, 0, &ops, opsCtx);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr, "hipObjPut failed: %s\n",
            hipObjGetErrorString(err.opError));
    exitCode = 1;
  } else {
    fprintf(stdout, "hipObjPut succeeded for %zu bytes\n", objSize);
  }

#if defined(HIPOBJ_HAVE_CURL)
  if (live) {
    curl_global_cleanup();
  }
#endif

  hipObjBufDeregister(devPtr);
  (void)hipFree(devPtr);
  hipObjShutdown();
  return exitCode;
}
