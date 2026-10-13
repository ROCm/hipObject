/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <span>
#include <string_view>

#include "hipobj.h"

namespace hipObj {

/* Passes token to sendRequest as is. The library passes an RdmaTokenHex,
 * which keeps a NUL after the digits for callbacks that expect one. */
int injectRdmaToken(hipObjOps_t *ops, void *ctx, std::string_view token);

/* Receives the reply into replyBuf through recvReply and decodes its RDMA
 * status. On success, reply is the part of replyBuf the callback filled. A
 * callback that reports more than replyBuf holds is an error. */
int receiveRdmaReplyRaw(hipObjOps_t *ops, void *ctx, std::span<char> replyBuf, std::string_view &reply,
                        int &rdmaStatus);

} // namespace hipObj
