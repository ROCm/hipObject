/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "control.h"

#include <cstring>

#include "token.h"

namespace hipObj {

int injectRdmaToken(hipObjOps_t* ops, void* ctx, const std::string& token) {
  if (!ops || !ops->sendRequest) {
    return -1;
  }
  return ops->sendRequest(ctx, token.c_str(), token.size());
}

int receiveRdmaReply(hipObjOps_t* ops, void* ctx, int& rdmaStatus) {
  char replyBuf[512];
  size_t replyLen = sizeof(replyBuf);
  return receiveRdmaReplyRaw(ops, ctx, replyBuf, &replyLen, rdmaStatus);
}

int receiveRdmaReplyRaw(hipObjOps_t* ops, void* ctx, char* replyBuf,
                        size_t* replyLen, int& rdmaStatus) {
  if (!ops || !ops->recvReply || !replyBuf || !replyLen) {
    return -1;
  }
  int ret = ops->recvReply(ctx, replyBuf, replyLen);
  if (ret != 0) {
    return ret;
  }
  return decodeRdmaReply(replyBuf, *replyLen, rdmaStatus) ? 0 : -1;
}

} // namespace hipObj
