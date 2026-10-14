/* SPDX-License-Identifier: MIT
 */

#include "v3-wire.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace hipObj::v3 {

namespace {

/* Splits `s` on `delim` into up to `maxFields` pieces; returns the number
 * actually found. A trailing empty field (s ends with delim) counts. */
size_t splitFields(const char* s, size_t len, char delim, std::string* fields,
                   size_t maxFields) {
  size_t count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= len && count < maxFields; ++i) {
    if (i == len || s[i] == delim) {
      fields[count++].assign(s + start, i - start);
      start = i + 1;
    }
  }
  return count;
}

bool parseHex64(const std::string& s, uint64_t& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  out = std::strtoull(s.c_str(), &end, 16);
  return end != nullptr && *end == '\0' && end != s.c_str();
}

bool parseHex32(const std::string& s, uint32_t& out) {
  uint64_t v = 0;
  if (!parseHex64(s, v) || v > 0xffffffffULL) {
    return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

bool parseHex16(const std::string& s, uint16_t& out) {
  uint64_t v = 0;
  if (!parseHex64(s, v) || v > 0xffffULL) {
    return false;
  }
  out = static_cast<uint16_t>(v);
  return true;
}

bool parseDecimalInt(const std::string& s, int& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

void bytesToHex(const uint8_t* bytes, size_t n, char* out /* n*2+1 bytes */) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0xf];
  }
  out[2 * n] = '\0';
}

bool hexToBytes(const std::string& s, uint8_t* out, size_t n) {
  if (s.size() != 2 * n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    char hex[3] = {s[2 * i], s[2 * i + 1], '\0'};
    char* end = nullptr;
    unsigned long v = std::strtoul(hex, &end, 16);
    if (end != hex + 2) {
      return false;
    }
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

/* IPv4-mapped IPv6 prefix (RFC 4291 2.5.5.2): bytes[0..9]=0, bytes[10..11]=
 * 0xff. */
constexpr uint8_t kV4MappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

/* token.listenIp (empty, dotted-quad IPv4, or colon-form IPv6) -> 16 raw
 * bytes in network order. Empty/unparseable -> all-zero ("::", unspecified).
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

bool decodeRdmaTokenV3(const char* headerValue, size_t headerValueLen,
                       RdmaTokenV3& out) {
  if (!headerValue) {
    return false;
  }

  /* ip-hex32 : port-hex4 : rkey-hex8 : remoteAddr-hex16 : length-hex16 --
   * 5 fields, all hex, kFieldDelim-delimited (see the file comment). */
  std::string fields[5];
  if (splitFields(headerValue, headerValueLen, kFieldDelim, fields, 5) != 5) {
    return false;
  }

  uint8_t ipBytes[16];
  if (!hexToBytes(fields[0], ipBytes, 16)) {
    return false;
  }
  std::string ipStr;
  if (!bytesToIpString(ipBytes, ipStr)) {
    return false;
  }

  RdmaTokenV3 parsed;
  parsed.listenIp = ipStr;
  if (!parseHex16(fields[1], parsed.listenPort)) {
    return false;
  }
  if (!parseHex32(fields[2], parsed.rkey)) {
    return false;
  }
  if (!parseHex64(fields[3], parsed.remoteAddr)) {
    return false;
  }
  if (!parseHex64(fields[4], parsed.length)) {
    return false;
  }
  out = parsed;
  return true;
}

std::string encodeRdmaReplyV3(const RdmaReplyV3& reply) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx",
               static_cast<unsigned long long>(reply.bytesTransferred));
  std::ostringstream oss;
  oss << reply.httpStatus << kFieldDelim << buf;
  return oss.str();
}

bool decodeRdmaReplyV3(const char* replyValue, size_t replyValueLen,
                       RdmaReplyV3& out) {
  if (!replyValue) {
    return false;
  }

  std::string fields[2];
  if (splitFields(replyValue, replyValueLen, kFieldDelim, fields, 2) != 2) {
    return false;
  }

  RdmaReplyV3 parsed;
  if (!parseDecimalInt(fields[0], parsed.httpStatus)) {
    return false;
  }
  if (!parseHex64(fields[1], parsed.bytesTransferred)) {
    return false;
  }
  out = parsed;
  return true;
}

} // namespace hipObj::v3
