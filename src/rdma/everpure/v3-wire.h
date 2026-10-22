/* SPDX-License-Identifier: MIT
 */

/* hipobj-rc-v3 wire format.
 *
 * v1 (see token.h) asks the peer to learn hipObject's QP number, GID
 * and LID from a hex-encoded token, then bring its own QP to RTR/RTS
 * against those values by hand. v3 instead has hipObject run an RDMA
 * CM listener (see rc-listener.h) and simply tells the peer where to
 * dial: RDMA CM negotiates the QP-level details itself as part of its
 * own connection setup, so none of that needs to go on the wire here.
 *
 * What v3 DOES need on the wire is the memory region for this specific
 * transfer (rkey/remoteAddr/length), since RDMA CM has no notion of
 * hipObject's buffer registrations, plus the listener's address so the
 * peer knows where to rdma_connect().
 *
 * Both messages are single ASCII lines, safe to carry verbatim as an
 * HTTP header value. No protocol tag/version prefix -- this is a PoC
 * with exactly one wire format in play, so there is nothing to
 * disambiguate against yet. Both share one field delimiter, kFieldDelim
 * (':'):
 *
 *   Token (hipObject -> peer, x-amz-rc-rdma-token):
 *     <ip-hex32>:<port-hex4>:<rkey-hex8>:<remoteAddr-hex16>:<length-hex16>
 *
 *     ip      32 hex digits: the 16 raw bytes of the RC-CM listener's
 *             address in network order, IPv4-mapped-IPv6 (RFC 4291
 *             2.5.5.2: bytes[0..9]=0, bytes[10..11]=0xff) for an IPv4
 *             listener, or a native IPv6 address otherwise. All-zero
 *             ("::") means unknown.
 *     port    4 hex digits, the TCP port the listener is bound to
 *     rkey    8 hex digits, the remote key for the memory region
 *     remoteAddr  16 hex digits, the memory region's base address
 *     length  16 hex digits, transfer length in bytes
 *
 *   Reply (peer -> hipObject, x-amz-rc-rdma-reply):
 *     <httpStatus>:<bytesTransferred-hex16>
 *
 *     httpStatus  3 decimal digits, 100 to 599; 200/204/206 succeed,
 *                 anything else fails
 *     bytesTransferred  16 hex digits, bytes the peer actually moved
 *
 * The peer is expected to already hold (or to establish, via the RC-CM
 * listener) an RC connection before or while sending its HTTP reply --
 * there is no peer QP/GID exchanged over HTTP in v3, so unlike v1
 * there is nothing here for hipObject to feed into a local QP
 * transition.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hipObj::v3 {

/*! Field delimiter shared by both x-amz-rc-rdma-token and
 *  x-amz-rc-rdma-reply. See the file comment above for the wire format
 *  each field actually takes. */
inline constexpr char kFieldDelim = ':';

struct RdmaTokenV3 {
    std::string listenIp{};  /* dotted-quad IPv4 or colon-form IPv6; empty when
                              * unknown. See encodeRdmaTokenV3Hex() for the wire
                              * encoding. */
    uint16_t listenPort = 0; /* 0 means no RC-CM listener is running */
    uint32_t rkey       = 0;
    uint64_t remoteAddr = 0;
    uint64_t length     = 0;
};

struct RdmaReplyV3 {
    int      httpStatus       = 0;
    uint64_t bytesTransferred = 0;
};

/*! Length of an encoded token: 32 + 4 + 8 + 16 + 16 hex digits and the four
 *  kFieldDelim between them. */
inline constexpr size_t kRdmaTokenV3HexLen = 80;

/*! An encoded token: kRdmaTokenV3HexLen characters and a terminating NUL,
 *  the same shape as v1's RdmaTokenHex (see token.h). */
using RdmaTokenV3Hex = std::array<char, kRdmaTokenV3HexLen + 1>;

/*! Encodes a RdmaTokenV3 as the x-amz-rc-rdma-token header value. */
RdmaTokenV3Hex encodeRdmaTokenV3Hex(const RdmaTokenV3 &token);

/*!
 * Decodes exactly kRdmaTokenV3HexLen characters, an x-amz-rc-rdma-token
 * header value as encodeRdmaTokenV3Hex() writes it. Returns std::nullopt
 * unless there are exactly five fields, each exactly its width in hex
 * digits (no sign, whitespace or "0x" prefix).
 */
std::optional<RdmaTokenV3> decodeRdmaTokenV3(std::string_view headerValue);

/*! Encodes a RdmaReplyV3 as the x-amz-rc-rdma-reply header value. */
std::string encodeRdmaReplyV3(const RdmaReplyV3 &reply);

/*!
 * Decodes an x-amz-rc-rdma-reply header value as encodeRdmaReplyV3() writes
 * it. The value comes from the network, so anything else is rejected:
 * std::nullopt unless there are exactly two fields, a 3-digit status from
 * 100 to 599 and exactly 16 hex digits. Trailing NUL, CR and LF characters
 * are ignored, as v1's reply parser does, for callbacks that report them.
 */
std::optional<RdmaReplyV3> decodeRdmaReplyV3(std::string_view replyValue);

} // namespace hipObj::v3
