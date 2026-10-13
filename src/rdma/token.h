/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* RDMA token encoding/decoding */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/* An RDMA token. Its wire layout, in docs/interop.rst, follows the sizes of
 * these fields, so changing one changes the wire format. */
struct RdmaToken {
    uint32_t      qpNum = 0;
    Gid           gid{};
    uint32_t      rkey       = 0;
    uint64_t      remoteAddr = 0;
    uint64_t      length     = 0;
    TransportType transport  = TransportType::DC;
    uint8_t       portNum    = 0;
    uint16_t      lid        = 0;
};

/* What an x-amz-rdma-reply value says about the transfer */
enum class RdmaReplyStatus {
    Success,      /* "ok", or a 200, 204, or 206 status */
    Failed,       /* "err", or any other status */
    NotSupported, /* a 501 status: the server doesn't support RDMA */
};

/* Doesn't allocate, so it's the one the transfer path uses */
RdmaTokenHex encodeRdmaTokenHex(const RdmaToken &token);

std::string encodeRdmaToken(const RdmaToken &token);

/* Decodes exactly kRdmaTokenHexLen hex digits. Returns std::nullopt for
 * anything else, or for a transport byte that isn't a TransportType. */
std::optional<RdmaToken> decodeRdmaTokenHex(std::string_view tokenHex);

/* The status of an x-amz-rdma-reply value, or std::nullopt when it's
 * malformed */
std::optional<RdmaReplyStatus> decodeRdmaReply(std::string_view reply);

/* The HTTP status in an x-amz-rdma-reply value (200 for "ok", -1 for
 * "err"), or std::nullopt when it's malformed */
std::optional<int> parseRdmaReplyHttpCode(std::string_view reply);

bool parseClientNicFromTokenHex(const char *tokenHex, char *nicIp, size_t nicIpLen);

/* The peer token in a "200:" reply, or std::nullopt when the reply is
 * malformed or doesn't carry one. Only a 200 reply carries a token. */
std::optional<RdmaToken> parsePeerTokenFromReply(std::string_view reply);

std::string encodeReplyWithPeerToken(int httpCode, const RdmaToken &peerToken);

} // namespace hipObj
