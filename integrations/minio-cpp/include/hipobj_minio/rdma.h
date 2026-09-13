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
inline constexpr const char* kAmzRdmaSession = "x-amz-rdma-session";
inline constexpr const char* kAmzRdmaProtocol = "x-amz-rdma-protocol";
inline constexpr const char* kAmzRdmaProtocolV2 = "hipobj-rc-v2";
inline constexpr const char* kAmzRdmaCancel = "x-amz-rdma-cancel";
inline constexpr const char* kAmzRdmaBytesTransferred =
  "x-amz-rdma-bytes-transferred";
inline constexpr const char* kAmzRdmaReplyHdr = "x-amz-rdma-reply";
inline constexpr const char* kAmzRdmaSessionHdr = "x-amz-rdma-session";
inline constexpr const char* kAmzRdmaCookieHdr = "x-amz-rdma-cookie";
inline constexpr const char* kAmzRdmaPsnHdr = "x-amz-rdma-psn";
inline constexpr const char* kAmzRdmaOpHdr = "x-amz-rdma-op";
inline constexpr const char* kAmzRdmaSizeHdr = "x-amz-rdma-size";
inline constexpr const char* kAmzRdmaOffsetHdr = "x-amz-rdma-offset";
inline constexpr const char* kAmzRdmaTargetHdr = "x-amz-rdma-target";
inline constexpr const char* kAmzRdmaQpnHdr = "x-amz-rdma-qpn";
inline constexpr const char* kAmzRdmaMrAddrHdr = "x-amz-rdma-mr-addr";
inline constexpr const char* kAmzRdmaMrRkeyHdr = "x-amz-rdma-mr-rkey";
inline constexpr const char* kControlPathPrepare = "/.hipobj-rc/prepare";
inline constexpr const char* kControlPathReady = "/.hipobj-rc/ready";
inline constexpr const char* kControlPathCancel = "/.hipobj-rc/cancel";

inline constexpr int kRdmaReplySuccess = 200;
inline constexpr int kRdmaReplyNoContent = 204;
inline constexpr int kRdmaReplyPartialContent = 206;
inline constexpr int kRdmaReplyNotImplemented = 501;

inline constexpr ssize_t kRdmaNotSupported = -2;
/* The v2 path failed for a reason other than "unsupported": the
 * transfer outcome is uncertain, so callers must not retry over
 * HTTP with the same buffer. */
inline constexpr ssize_t kRdmaV2Failed = -3;

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
