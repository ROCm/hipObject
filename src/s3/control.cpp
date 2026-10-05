/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "control.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "hipobj-span.h"
#include "token.h"

namespace hipObj {

bool
injectRdmaToken(const hipObjOps_t &ops, void *ctx, std::string_view token)
{
    if (ops.sendRequest == nullptr) {
        return false;
    }
    return ops.sendRequest(ctx, token.data(), token.size()) == 0;
}

std::optional<RdmaReply>
receiveRdmaReplyRaw(const hipObjOps_t &ops, void *ctx, std::span<char> replyBuf)
{
    if (ops.recvReply == nullptr || replyBuf.empty()) {
        return std::nullopt;
    }
    size_t replyLen = replyBuf.size();
    if (ops.recvReply(ctx, replyBuf.data(), &replyLen) != 0) {
        return std::nullopt;
    }
    if (replyLen > replyBuf.size()) {
        return std::nullopt;
    }
    const std::string_view received = asStringView(replyBuf.first(replyLen));
    const auto             status   = decodeRdmaReply(received);
    if (!status) {
        return std::nullopt;
    }
    return RdmaReply{.text = received, .status = *status};
}

int receiveRdmaReplyV3(hipObjOps_t* ops, void* ctx, v3::RdmaReplyV3& reply) {
  if (!ops || !ops->recvReply) {
    return -1;
  }
  char replyBuf[512];
  size_t replyLen = sizeof(replyBuf);
  int ret = ops->recvReply(ctx, replyBuf, &replyLen);
  if (ret != 0) {
    return ret;
  }
  if (replyLen > sizeof(replyBuf)) {
    return -1;
  }
  return v3::decodeRdmaReplyV3(replyBuf, replyLen, reply) ? 0 : -1;
}

} // namespace hipObj
