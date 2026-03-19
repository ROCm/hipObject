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

} // namespace hipObj
