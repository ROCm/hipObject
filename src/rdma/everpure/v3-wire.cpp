/* SPDX-License-Identifier: MIT
 */

#include "v3-wire.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <string_view>

#include "hipobj-parse.h"

namespace hipObj::v3 {

namespace {

/* The kFieldDelim-separated fields of `s`, or std::nullopt unless there are
 * exactly N: fewer is a missing field, more is input the decoder would
 * otherwise ignore. */
template <size_t N>
std::optional<std::array<std::string_view, N>> splitExactly(std::string_view s) {
  std::array<std::string_view, N> fields{};
  for (size_t i = 0; i + 1 < N; ++i) {
    const size_t delim = s.find(kFieldDelim);
    if (delim == std::string_view::npos) {
      return std::nullopt;
    }
    fields[i] = s.substr(0, delim);
    s.remove_prefix(delim + 1);
  }
  if (s.find(kFieldDelim) != std::string_view::npos) {
    return std::nullopt;
  }
  fields[N - 1] = s;
  return fields;
}

/* Exactly `width` hex digits as a T. parseNumber() rejects a sign,
 * whitespace and a "0x" prefix, and a value that doesn't fit in T. */
template <typename T>
std::optional<T> hexField(std::string_view s, size_t width) {
  if (s.size() != width) {
    return std::nullopt;
  }
  return parseNumber<T>(s, 16);
}

/* The 16 bytes of the token's ip field: exactly 32 hex digits */
std::optional<std::array<uint8_t, 16>> ipBytesField(std::string_view s) {
  std::array<uint8_t, 16> bytes{};
  if (s.size() != 2 * bytes.size()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < bytes.size(); ++i) {
    const auto byte = hexField<uint8_t>(s.substr(2 * i, 2), 2);
    if (!byte) {
      return std::nullopt;
    }
    bytes[i] = *byte;
  }
  return bytes;
}

void bytesToHex(const uint8_t* bytes, size_t n, char* out /* n*2+1 bytes */) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0xf];
  }
  out[2 * n] = '\0';
}

/* IPv4-mapped IPv6 prefix (RFC 4291 2.5.5.2): bytes[0..9]=0, bytes[10..11]=
 * 0xff. */
constexpr uint8_t kV4MappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

/* token.listenIp (empty, dotted-quad IPv4, or colon-form IPv6) -> 16 raw
 * bytes in network order. Empty/unparsable -> all-zero ("::", unspecified).
 */
void ipStringToBytes(const std::string& ip, uint8_t out[16]) {
  std::memset(out, 0, 16);
  if (ip.empty()) {
    return;
  }
  struct in_addr v4;
  if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
    std::memcpy(out, kV4MappedPrefix, 12);
    std::memcpy(out + 12, &v4, 4);
    return;
  }
  struct in6_addr v6;
  if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) {
    std::memcpy(out, &v6, 16);
  }
}

/* Reverse of ipStringToBytes(): 16 raw bytes -> dotted-quad IPv4 (if
 * IPv4-mapped), colon-form IPv6, or empty (all-zero). */
bool bytesToIpString(const uint8_t in[16], std::string& ip) {
  static constexpr uint8_t kZero[16] = {0};
  if (std::memcmp(in, kZero, 16) == 0) {
    ip.clear();
    return true;
  }
  char buf[INET6_ADDRSTRLEN];
  if (std::memcmp(in, kV4MappedPrefix, 12) == 0) {
    struct in_addr v4;
    std::memcpy(&v4, in + 12, 4);
    if (!inet_ntop(AF_INET, &v4, buf, sizeof(buf))) {
      return false;
    }
  } else {
    struct in6_addr v6;
    std::memcpy(&v6, in, 16);
    if (!inet_ntop(AF_INET6, &v6, buf, sizeof(buf))) {
      return false;
    }
  }
  ip = buf;
  return true;
}

} // namespace

RdmaTokenV3Hex encodeRdmaTokenV3Hex(const RdmaTokenV3& token) {
  uint8_t ipBytes[16];
  ipStringToBytes(token.listenIp, ipBytes);
  char ipHex[33];
  bytesToHex(ipBytes, 16, ipHex);

  /* Every field is fixed-width, so the result is always exactly
   * kRdmaTokenV3HexLen characters and fits with its NUL. Zeroed first, so
   * nothing but the token's own characters is ever left in the array. */
  RdmaTokenV3Hex out{};
  std::snprintf(out.data(), out.size(), "%s%c%04x%c%08x%c%016llx%c%016llx",
                ipHex, kFieldDelim, static_cast<unsigned>(token.listenPort),
                kFieldDelim, static_cast<unsigned>(token.rkey), kFieldDelim,
                static_cast<unsigned long long>(token.remoteAddr), kFieldDelim,
                static_cast<unsigned long long>(token.length));
  return out;
}

std::optional<RdmaTokenV3> decodeRdmaTokenV3(std::string_view headerValue) {
  /* ip-hex32 : port-hex4 : rkey-hex8 : remoteAddr-hex16 : length-hex16 --
   * 5 fields, all hex, kFieldDelim-delimited (see the file comment). */
  if (headerValue.size() != kRdmaTokenV3HexLen) {
    return std::nullopt;
  }
  const auto fields = splitExactly<5>(headerValue);
  if (!fields) {
    return std::nullopt;
  }
  const auto ipBytes = ipBytesField((*fields)[0]);
  const auto port = hexField<uint16_t>((*fields)[1], 4);
  const auto rkey = hexField<uint32_t>((*fields)[2], 8);
  const auto remoteAddr = hexField<uint64_t>((*fields)[3], 16);
  const auto length = hexField<uint64_t>((*fields)[4], 16);
  if (!ipBytes || !port || !rkey || !remoteAddr || !length) {
    return std::nullopt;
  }

  RdmaTokenV3 token;
  if (!bytesToIpString(ipBytes->data(), token.listenIp)) {
    return std::nullopt;
  }
  token.listenPort = *port;
  token.rkey = *rkey;
  token.remoteAddr = *remoteAddr;
  token.length = *length;
  return token;
}

std::string encodeRdmaReplyV3(const RdmaReplyV3& reply) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx",
               static_cast<unsigned long long>(reply.bytesTransferred));
  std::ostringstream oss;
  oss << reply.httpStatus << kFieldDelim << buf;
  return oss.str();
}

std::optional<RdmaReplyV3> decodeRdmaReplyV3(std::string_view replyValue) {
  /* A callback may report trailing NUL, CR and LF characters; v1's reply
   * parser (token.cpp) ignores them too */
  while (!replyValue.empty() &&
         (replyValue.back() == '\0' || replyValue.back() == '\n' ||
          replyValue.back() == '\r')) {
    replyValue.remove_suffix(1);
  }
  /* httpStatus-decimal3 : bytesTransferred-hex16 */
  const auto fields = splitExactly<2>(replyValue);
  if (!fields) {
    return std::nullopt;
  }
  /* Exactly 3 digits, as HTTP status codes are; unsigned, so a sign is
   * rejected too */
  const std::string_view statusField = (*fields)[0];
  const auto status = statusField.size() == 3
                          ? parseNumber<unsigned>(statusField)
                          : std::nullopt;
  if (!status || *status < 100 || *status > 599) {
    return std::nullopt;
  }
  const auto bytesTransferred = hexField<uint64_t>((*fields)[1], 16);
  if (!bytesTransferred) {
    return std::nullopt;
  }
  RdmaReplyV3 reply;
  reply.httpStatus = static_cast<int>(*status);
  reply.bytesTransferred = *bytesTransferred;
  return reply;
}

} // namespace hipObj::v3
