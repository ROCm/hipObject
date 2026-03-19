/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hipobj.h"

#include <string>

namespace hipObj {

int injectRdmaToken(hipObjOps_t* ops, void* ctx,
                   const std::string& token);
int receiveRdmaReply(hipObjOps_t* ops, void* ctx,
                     int& rdmaStatus);

} // namespace hipObj
