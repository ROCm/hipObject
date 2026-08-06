/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

#include "hipobj.h"

namespace hipObj {

int injectRdmaToken(hipObjOps_t* ops, void* ctx, const std::string& token);
int receiveRdmaReply(hipObjOps_t* ops, void* ctx, int& rdmaStatus);
int receiveRdmaReplyRaw(hipObjOps_t* ops, void* ctx, char* replyBuf,
                        size_t* replyLen, int& rdmaStatus);

} // namespace hipObj
