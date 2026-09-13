/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Internal v2 client entry points (called from src/hipobj.cpp under
 * v2::apiLock()). */

#pragma once

#include <cstdint>

#include "buffer.h"
#include "hipobj.h"

namespace hipObj {

/* The library-wide buffer map, defined in src/hipobj.cpp. */
extern BufferMap g_bufferMap;

namespace v2 {

/* hipObjInitV2 body. Returns a hipObjOpError_t value. */
int v2Init(hipObjConfigV2_t* config);

/* hipObjShutdown path for the v2 half. Returns a hipObjOpError_t
 * value; hipObjRdmaError when leftover poison stopped the teardown. */
int v2Shutdown();

/* Shared body of hipObjGetV2/hipObjPutV2 (apiLock held by the
 * caller). Returns a hipObjOpError_t value. */
int v2Transfer(int isPut, const char* bucket, const char* key, void* devPtr,
               uint64_t size, uint64_t offset, const char* query,
               hipObjOpsV2_t* ops, void* ctx);

} // namespace v2
} // namespace hipObj
