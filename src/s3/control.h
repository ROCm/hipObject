/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "hipobj.h"
#include "v3-wire.h"

namespace hipObj {

enum class RdmaReplyStatus; /* in token.h */

/* A reply received through recvReply */
struct RdmaReply {
    std::string_view text;   /* the part of the reply buffer the callback filled */
    RdmaReplyStatus  status; /* decoded from text */
};

/* Passes token to sendRequest as is. The library passes an RdmaTokenHex,
 * which keeps a NUL after the digits for callbacks that expect one.
 * Returns false when there's no callback or it fails. */
[[nodiscard]] bool injectRdmaToken(const hipObjOps_t &ops, void *ctx, std::string_view token);

/* Receives the reply into replyBuf through recvReply and decodes its RDMA
 * status. Returns std::nullopt when there's no callback, the callback fails
 * or reports more than replyBuf holds, or the reply is malformed. */
std::optional<RdmaReply> receiveRdmaReplyRaw(const hipObjOps_t &ops, void *ctx, std::span<char> replyBuf);

/*! hipobj-rc-v3 counterpart of receiveRdmaReplyRaw(): receives the reply
 * through recvReply and decodes it as a v3::RdmaReplyV3. Returns
 * std::nullopt when the callback fails or reports more than the buffer
 * holds, or the reply is malformed (see v3::decodeRdmaReplyV3()). */
std::optional<v3::RdmaReplyV3> receiveRdmaReplyV3(hipObjOps_t* ops, void* ctx);

} // namespace hipObj
