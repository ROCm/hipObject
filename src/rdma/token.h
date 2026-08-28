/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ibv-core.h"

namespace hipObj {

enum TransportType : uint8_t {
  TRANSPORT_DC = 0x00,
  TRANSPORT_RC = 0x01,
};

struct RdmaToken {
  uint32_t qpNum;
  uint8_t gid[16];
  uint32_t rkey;
  uint64_t remoteAddr;
  uint64_t length;
  uint8_t transport;
  uint8_t portNum;
  uint16_t lid;
};

std::string encodeRdmaToken(const RdmaToken& token);

bool decodeRdmaTokenHex(const char* tokenHex, RdmaToken& out);

bool decodeRdmaReply(const char* reply, size_t replyLen, int& status);

bool parseRdmaReplyHttpCode(const char* reply, size_t replyLen, int& httpCode);

bool parseClientNicFromTokenHex(const char* tokenHex, char* nicIp,
                                size_t nicIpLen);

std::string formatRdmaHeaderValue(const char* tokenHex, const void* buf,
                                  size_t size);

bool parsePeerTokenFromReply(const char* reply, size_t replyLen,
                             RdmaToken& peerToken, int& httpCode);

// v2 protocol: parse PREPARE reply that carries both the server token and a
// session ID used to correlate the subsequent READY request.
// Reply format: "<httpCode>:<tokenHex>:<sessionId>"
bool parsePrepareReply(const char* reply, size_t replyLen, RdmaToken& peerToken,
                       int& httpCode, std::string& sessionId);

std::string encodeReplyWithPeerToken(int httpCode, const RdmaToken& peerToken);
std::string encodeReplyWithPeerTokenAndSession(int httpCode,
                                               const RdmaToken& peerToken,
                                               const std::string& sessionId);

} // namespace hipObj
