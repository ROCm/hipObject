/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "hipobj_minio/context.h"

namespace hipobj::minio {

inline constexpr const char* kEmptySha256 =
  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
inline constexpr const char* kUnsignedPayload = "UNSIGNED-PAYLOAD";

inline constexpr const char* kAmzRdmaToken = "x-amz-rdma-token";
inline constexpr const char* kAmzRdmaReply = "x-amz-rdma-reply";
inline constexpr const char* kAmzRdmaBytesTransferred =
  "x-amz-rdma-bytes-transferred";

inline constexpr int kRdmaReplySuccess = 200;
inline constexpr int kRdmaReplyNoContent = 204;
inline constexpr int kRdmaReplyPartialContent = 206;
inline constexpr int kRdmaReplyNotImplemented = 501;

inline constexpr ssize_t kRdmaNotSupported = -2;

inline constexpr long kRdmaConnectTimeoutSecs = 5;
inline constexpr long kRdmaTimeoutSecs = 10;
inline constexpr int kRdmaMaxAttempts = 2;

ssize_t rdmaPut(S3RdmaContext* ctx, const char* token, const void* buf,
                size_t size);

ssize_t rdmaGet(S3RdmaContext* ctx, const char* token, const void* buf,
                size_t size);

ssize_t rdmaPutWithRetry(S3RdmaContext* ctx, void* buf, size_t size);

ssize_t rdmaGetWithRetry(S3RdmaContext* ctx, void* buf, size_t size);

} // namespace hipobj::minio
