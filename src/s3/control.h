/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string_view>

#include "hipobj.h"

namespace hipObj {

/* Passes token to sendRequest as is. The library passes an RdmaTokenHex,
 * which keeps a NUL after the digits for callbacks that expect one. */
int injectRdmaToken(hipObjOps_t *ops, void *ctx, std::string_view token);
int receiveRdmaReplyRaw(hipObjOps_t *ops, void *ctx, char *replyBuf, size_t *replyLen, int &rdmaStatus);

} // namespace hipObj
