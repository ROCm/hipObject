/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RDMA token encoding/decoding implementation.
 */

#include "token.hpp"

#include <cstring>
#include <sstream>

namespace hipObj {

namespace {

const char HEX[] = "0123456789abcdef";

} // namespace

std::string encodeRdmaToken(const RdmaToken& token) {
  uint8_t buf[1 + 4 + 16 + 4 + 8 + 8 + 1 + 2];
  size_t off = 0;

  buf[off++] = token.transport;
  std::memcpy(buf + off, &token.qpNum, 4);
  off += 4;
  std::memcpy(buf + off, token.gid, 16);
  off += 16;
  std::memcpy(buf + off, &token.rkey, 4);
  off += 4;
  std::memcpy(buf + off, &token.remoteAddr, 8);
  off += 8;
  std::memcpy(buf + off, &token.length, 8);
  off += 8;
  buf[off++] = token.portNum;
  std::memcpy(buf + off, &token.lid, 2);
  off += 2;

  std::ostringstream oss;
  for (size_t i = 0; i < off; ++i) {
    oss << HEX[(buf[i] >> 4) & 0xf] << HEX[buf[i] & 0xf];
  }
  return oss.str();
}

bool decodeRdmaReply(const char* reply, size_t replyLen, int& status) {
  if (!reply || replyLen < 2)
    return false;
  if (replyLen >= 2 && reply[0] == 'o' && reply[1] == 'k' &&
      (replyLen == 2 || reply[2] == '\0' || reply[2] == '\n')) {
    status = 0;
    return true;
  }
  if (replyLen >= 3 && reply[0] == 'e' && reply[1] == 'r' && reply[2] == 'r' &&
      (replyLen == 3 || reply[3] == '\0' || reply[3] == '\n')) {
    status = -1;
    return true;
  }
  return false;
}

} // namespace hipObj
