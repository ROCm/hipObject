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

int
injectRdmaToken(hipObjOps_t *ops, void *ctx, std::string_view token)
{
    if (!ops || !ops->sendRequest) {
        return -1;
    }
    return ops->sendRequest(ctx, token.data(), token.size());
}

std::optional<RdmaReply>
receiveRdmaReplyRaw(hipObjOps_t *ops, void *ctx, std::span<char> replyBuf)
{
    if (!ops || !ops->recvReply || replyBuf.empty()) {
        return std::nullopt;
    }
    size_t replyLen = replyBuf.size();
    if (ops->recvReply(ctx, replyBuf.data(), &replyLen) != 0) {
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
    return RdmaReply{received, *status};
}

} // namespace hipObj
