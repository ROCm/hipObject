/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>

#include "hipobj.h"

namespace hipObj {

int injectRdmaToken(hipObjOps_t *ops, void *ctx, const std::string &token);
int receiveRdmaReplyRaw(hipObjOps_t *ops, void *ctx, char *replyBuf, size_t *replyLen, int &rdmaStatus);

} // namespace hipObj
