/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>

#include "hipobj.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char* endpoint;
  const char* bucket;
  const char* object;
  const char* accessKey;
  const char* secretKey;
  int isPut;
  size_t objectSize;
  const void* devPtr;
  char lastReply[512];
} hipObjS3CurlCtx;

int hipObjS3CurlSendRequest(void* ctx, const char* token, size_t tokenLen);
int hipObjS3CurlRecvReply(void* ctx, char* reply, size_t* replyLen);

#ifdef __cplusplus
}
#endif
