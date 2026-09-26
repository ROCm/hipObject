/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/* Consumers that already include the real verbs.h (test clients
 * linking libibverbs) get the ibv types from there; the shipped
 * library uses the vendored core definitions. */
#if !defined(HIPOBJ_REAL_VERBS)
#include "ibv-core.h"
#endif

namespace hipObj {

enum TransportType : uint8_t {
  TRANSPORT_DC = 0x00,
  TRANSPORT_RC = 0x01,
};

struct RdmaToken {
  uint32_t qpNum = 0;
  uint8_t gid[16] = {};
  uint32_t rkey = 0;
  uint64_t remoteAddr = 0;
  uint64_t length = 0;
  uint8_t transport = 0;
  uint8_t portNum = 0;
  uint16_t lid = 0;
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

std::string encodeReplyWithPeerToken(int httpCode, const RdmaToken& peerToken);

} // namespace hipObj
