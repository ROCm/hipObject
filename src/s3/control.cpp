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

} // namespace hipObj
