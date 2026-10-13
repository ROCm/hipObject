/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "gid.h"

namespace hipObj {

/* Hex digits in an encoded token: two for each of its 44 bytes */
inline constexpr size_t kRdmaTokenHexLen = 88;

/* An encoded token: kRdmaTokenHexLen hex digits and a terminating NUL */
using RdmaTokenHex = std::array<char, kRdmaTokenHexLen + 1>;

/* The token's transport byte on the wire */
enum class TransportType : uint8_t {
    DC = 0x00,
    RC = 0x01,
};

struct RdmaToken {
    uint32_t      qpNum      = 0;
    Gid           gid        = {};
    uint32_t      rkey       = 0;
    uint64_t      remoteAddr = 0;
    uint64_t      length     = 0;
    TransportType transport  = TransportType::DC;
    uint8_t       portNum    = 0;
    uint16_t      lid        = 0;
};

/* Doesn't allocate, so it's the one the transfer path uses */
RdmaTokenHex encodeRdmaTokenHex(const RdmaToken &token);

std::string encodeRdmaToken(const RdmaToken &token);

bool decodeRdmaTokenHex(const char *tokenHex, RdmaToken &out);

bool decodeRdmaReply(std::string_view reply, int &status);

bool parseRdmaReplyHttpCode(std::string_view reply, int &httpCode);

bool parseClientNicFromTokenHex(const char *tokenHex, char *nicIp, size_t nicIpLen);

std::string formatRdmaHeaderValue(const char *tokenHex, const void *buf, size_t size);

bool parsePeerTokenFromReply(std::string_view reply, RdmaToken &peerToken, int &httpCode);

std::string encodeReplyWithPeerToken(int httpCode, const RdmaToken &peerToken);

} // namespace hipObj
