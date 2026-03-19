/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "control.hpp"

#include <cstring>

#include "token.hpp"

namespace hipObj {

int injectRdmaToken(hipObjOps_t* ops, void* ctx, const std::string& token) {
  if (!ops || !ops->sendRequest) {
    return -1;
  }
  return ops->sendRequest(ctx, token.c_str(), token.size());
}

int receiveRdmaReply(hipObjOps_t* ops, void* ctx, int& rdmaStatus) {
  if (!ops || !ops->recvReply) {
    return -1;
  }
  char replyBuf[256];
  std::memset(replyBuf, 0, sizeof(replyBuf));
  size_t replyLen = sizeof(replyBuf);
  int ret = ops->recvReply(ctx, replyBuf, &replyLen);
  if (ret != 0) {
    return ret;
  }
  return decodeRdmaReply(replyBuf, replyLen, rdmaStatus) ? 0 : -1;
}

} // namespace hipObj
