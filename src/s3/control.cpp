/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "control.h"

#include <cstddef>
#include <span>
#include <string_view>

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

int
receiveRdmaReplyRaw(hipObjOps_t *ops, void *ctx, std::span<char> replyBuf, std::string_view &reply,
                    int &rdmaStatus)
{
    if (!ops || !ops->recvReply || replyBuf.empty()) {
        return -1;
    }
    size_t replyLen = replyBuf.size();
    int    ret      = ops->recvReply(ctx, replyBuf.data(), &replyLen);
    if (ret != 0) {
        return ret;
    }
    if (replyLen > replyBuf.size()) {
        return -1;
    }
    const std::string_view received(replyBuf.data(), replyLen);
    if (!decodeRdmaReply(received, rdmaStatus)) {
        return -1;
    }
    reply = received;
    return 0;
}

} // namespace hipObj
