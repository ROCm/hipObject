/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Example: download an S3 object directly into
 *          GPU VRAM via RDMA.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <hip/hip_runtime.h>

#include "hipobj.h"

static int stubSendRequest(
    void* ctx, const char* token,
    size_t tokenLen) {
  (void)ctx;
  fprintf(stderr,
    "[get-object] would send RDMA token "
    "(%zu bytes) via S3 GET x-amz-rdma-token\n",
    tokenLen);
  return 0;
}

static int stubRecvReply(
    void* ctx, char* reply,
    size_t* replyLen) {
  (void)ctx;
  const char* ok = "ok";
  size_t len = strlen(ok);
  if (*replyLen < len) {
    return -1;
  }
  memcpy(reply, ok, len);
  *replyLen = len;
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr,
      "Usage: %s <object-size-bytes>\n",
      argv[0]);
    return 1;
  }

  size_t objSize =
    static_cast<size_t>(atol(argv[1]));
  if (objSize == 0) {
    fprintf(stderr,
      "Invalid object size: %s\n", argv[1]);
    return 1;
  }

  hipObjConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.endpoint = "https://s3.example.com";
  cfg.region = "us-east-1";
  cfg.gpuDevice = 0;

  hipObjError_t err = hipObjInit(&cfg);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr,
      "hipObjInit failed: %s\n",
      hipObjGetErrorString(err.opError));
    return 1;
  }

  void* devPtr = nullptr;
  hipError_t hip_err =
    hipMalloc(&devPtr, objSize);
  if (hip_err != hipSuccess) {
    fprintf(stderr,
      "hipMalloc failed: %d\n", hip_err);
    hipObjShutdown();
    return 1;
  }

  err = hipObjBufRegister(devPtr, objSize);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr,
      "hipObjBufRegister failed: %s\n",
      hipObjGetErrorString(err.opError));
    (void)hipFree(devPtr);
    hipObjShutdown();
    return 1;
  }

  hipObjOps_t ops;
  ops.sendRequest = stubSendRequest;
  ops.recvReply = stubRecvReply;

  err = hipObjGet(
    nullptr, devPtr, objSize, 0, &ops, nullptr);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr,
      "hipObjGet failed: %s\n",
      hipObjGetErrorString(err.opError));
  } else {
    fprintf(stdout,
      "hipObjGet succeeded for %zu bytes\n",
      objSize);
  }

  hipObjBufDeregister(devPtr);
  (void)hipFree(devPtr);
  hipObjShutdown();
  return 0;
}
