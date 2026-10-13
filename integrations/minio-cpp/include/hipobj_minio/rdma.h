/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "hipobj_minio/context.h"

namespace hipobj::minio {

inline constexpr const char *kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
inline constexpr const char *kUnsignedPayload = "UNSIGNED-PAYLOAD";

inline constexpr const char *kAmzRdmaToken    = "x-amz-rdma-token";
inline constexpr const char *kAmzRdmaReply    = "x-amz-rdma-reply";
inline constexpr const char *kAmzRdmaSession  = "x-amz-rdma-session";
inline constexpr const char *kAmzRdmaProtocol = "x-amz-rdma-protocol";
#ifdef HIPOBJECT_V2_API
inline constexpr const char *kAmzRdmaProtocolV2 = "hipobj-rc-v2";
#endif
inline constexpr const char *kAmzRdmaCancel           = "x-amz-rdma-cancel";
inline constexpr const char *kAmzRdmaBytesTransferred = "x-amz-rdma-bytes-transferred";

inline constexpr int kRdmaReplySuccess        = 200;
inline constexpr int kRdmaReplyNoContent      = 204;
inline constexpr int kRdmaReplyPartialContent = 206;
inline constexpr int kRdmaReplyNotImplemented = 501;

inline constexpr long kRdmaConnectTimeoutSecs = 5;
inline constexpr long kRdmaTimeoutSecs        = 10;
inline constexpr int  kRdmaMaxAttempts        = 2;

/* How an RDMA transfer ended */
enum class RdmaStatus {
    Ok,
    Failed,
    NotSupported, /* the server can't do this transfer by RDMA; use HTTP */
};

/* The outcome of an RDMA transfer, and the bytes it moved when it
 * succeeded */
struct RdmaResult {
    RdmaStatus status = RdmaStatus::Failed;
    size_t     bytes  = 0;
};

RdmaResult rdmaPut(S3RdmaContext *ctx, const char *token, size_t size);

RdmaResult rdmaGet(S3RdmaContext *ctx, const char *token, size_t size);

RdmaResult rdmaPutWithRetry(S3RdmaContext *ctx, void *buf, size_t size);

RdmaResult rdmaGetWithRetry(S3RdmaContext *ctx, void *buf, size_t size);

} // namespace hipobj::minio
