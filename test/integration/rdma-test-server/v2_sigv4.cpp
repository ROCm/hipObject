/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_sigv4.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace hipObj {
namespace v2 {

namespace {

std::string toLowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string trimCopy(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\r')) {
    --e;
  }
  return s.substr(b, e - b);
}

std::string hex(const unsigned char* data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0xf]);
  }
  return out;
}

std::string sha256Hex(const std::string& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest);
  return hex(digest, sizeof(digest));
}

std::string hmacSha256(const std::string& key, const std::string& msg) {
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), mac,
       &len);
  return std::string(reinterpret_cast<char*>(mac), len);
}

/* Splits "Credential=AKID/date/region/service/aws4_request". */
bool parseCredential(const std::string& auth, std::string& accessKey,
                     std::string& date, std::string& region) {
  const std::string needle = "Credential=";
  size_t pos = auth.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  size_t start = pos + needle.size();
  size_t end = auth.find(',', start);
  std::string cred = auth.substr(start, end == std::string::npos
                                          ? std::string::npos
                                          : end - start);
  /* AKID/date/region/service/terminator */
  size_t p1 = cred.find('/');
  if (p1 == std::string::npos) {
    return false;
  }
  size_t p2 = cred.find('/', p1 + 1);
  if (p2 == std::string::npos) {
    return false;
  }
  size_t p3 = cred.find('/', p2 + 1);
  if (p3 == std::string::npos) {
    return false;
  }
  accessKey = cred.substr(0, p1);
  date = cred.substr(p1 + 1, p2 - p1 - 1);
  region = cred.substr(p2 + 1, p3 - p2 - 1);
  return !accessKey.empty();
}

std::string parseSignedHeaders(const std::string& auth) {
  const std::string needle = "SignedHeaders=";
  size_t pos = auth.find(needle);
  if (pos == std::string::npos) {
    return std::string();
  }
  size_t start = pos + needle.size();
  size_t end = auth.find(',', start);
  return auth.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
}

std::string parseSignature(const std::string& auth) {
  const std::string needle = "Signature=";
  size_t pos = auth.rfind(needle);
  if (pos == std::string::npos) {
    return std::string();
  }
  return auth.substr(pos + needle.size());
}

/* Rebuilds the canonical header list from the raw block using only
 * the names listed in SignedHeaders. */
bool canonicalHeaders(const std::string& raw, const std::string& signedList,
                      std::string& canonicalOut) {
  std::map<std::string, std::string> present;
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t lineEnd = raw.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      lineEnd = raw.size();
    }
    std::string line = raw.substr(pos, lineEnd - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      present[toLowerCopy(trimCopy(line.substr(0, colon)))] = trimCopy(
        line.substr(colon + 1));
    }
    pos = lineEnd + 2;
  }
  std::stringstream ss(signedList);
  std::string name;
  canonicalOut.clear();
  while (std::getline(ss, name, ';')) {
    if (name.empty()) {
      continue;
    }
    auto it = present.find(name);
    if (it == present.end()) {
      return false;
    }
    canonicalOut += name;
    canonicalOut += ':';
    canonicalOut += it->second;
    canonicalOut += '\n';
  }
  return !canonicalOut.empty();
}

} // namespace

std::string extractAuthorization(const std::string& rawHeaders) {
  const std::string needle = "authorization:";
  std::string lower = toLowerCopy(rawHeaders);
  size_t pos = 0;
  while (pos < lower.size()) {
    size_t lineEnd = lower.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      lineEnd = lower.size();
    }
    size_t lineLen = lineEnd - pos;
    if (lineLen >= needle.size() &&
        lower.compare(pos, needle.size(), needle) == 0) {
      std::string v = rawHeaders.substr(pos + needle.size(),
                                        lineLen - needle.size());
      return trimCopy(v);
    }
    pos = lineEnd + 2;
  }
  return std::string();
}

BuiltinVerifier::BuiltinVerifier(std::string accessKey, std::string secretKey,
                                 std::string region)
  : accessKey_(std::move(accessKey)), secretKey_(std::move(secretKey)),
    region_(std::move(region)) {
}

std::string BuiltinVerifier::sign(
  const std::string& method, const std::string& uri,
  const std::map<std::string, std::string>& headers, const std::string& body,
  const std::string& amzDate) const {
  std::map<std::string, std::string> canon;
  for (const auto& [k, v] : headers) {
    canon[toLowerCopy(k)] = v;
  }
  std::string signedList;
  {
    std::vector<std::string> names;
    for (const auto& [k, v] : canon) {
      names.push_back(k);
    }
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i < names.size(); ++i) {
      signedList += names[i];
      if (i + 1 < names.size()) {
        signedList += ";";
      }
    }
  }
  std::string canonicalHeaders;
  for (const auto& [k, v] : canon) {
    canonicalHeaders += k;
    canonicalHeaders += ':';
    canonicalHeaders += v;
    canonicalHeaders += '\n';
  }
  std::string payloadHash = sha256Hex(body);
  std::string amzDateKey = "x-amz-date";
  std::string date = amzDate;
  if (date.size() >= 8) {
    date = date.substr(0, 8);
  }
  std::string scope = date + "/" + region_ + "/hipobj/aws4_request";
  std::string canonicalRequest = method + "\n" + uri + "\n" +
                                 std::string() /* no query */ + "\n" +
                                 canonicalHeaders + "\n" + signedList + "\n" +
                                 payloadHash;
  std::string toSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" +
                       sha256Hex(canonicalRequest);
  std::string kDate = hmacSha256("AWS4" + secretKey_, date.substr(0, 8));
  std::string kRegion = hmacSha256(kDate, region_);
  std::string kService = hmacSha256(kRegion, "hipobj");
  std::string kSigning = hmacSha256(kService, "aws4_request");
  std::string signature = hex(reinterpret_cast<const unsigned char*>(
                                hmacSha256(kSigning, toSign).data()),
                              32);
  (void)amzDateKey;
  return "AWS4-HMAC-SHA256 Credential=" + accessKey_ + "/" + scope +
         ", SignedHeaders=" + signedList + ", Signature=" + signature;
}

std::optional<VerifiedCredential> BuiltinVerifier::verify(
  const std::string& method, const std::string& uri,
  const std::string& rawHeaderBlock, const std::string& body) {
  std::string auth = extractAuthorization(rawHeaderBlock);
  if (auth.empty() || auth.find("AWS4-HMAC-SHA256") != 0) {
    return std::nullopt;
  }
  std::string key;
  std::string date;
  std::string region;
  if (!parseCredential(auth, key, date, region) || key != accessKey_) {
    return std::nullopt;
  }
  std::string signedList = parseSignedHeaders(auth);
  std::string providedSig = parseSignature(auth);
  if (signedList.empty() || providedSig.empty()) {
    return std::nullopt;
  }
  std::string canonicalHeaderLines;
  if (!canonicalHeaders(rawHeaderBlock, signedList, canonicalHeaderLines)) {
    return std::nullopt;
  }
  /* x-amz-date from the raw block (signed). */
  std::string amzDate;
  {
    std::map<std::string, std::string> present;
    size_t pos = 0;
    const std::string raw = rawHeaderBlock;
    while (pos < raw.size()) {
      size_t lineEnd = raw.find("\r\n", pos);
      if (lineEnd == std::string::npos) {
        lineEnd = raw.size();
      }
      std::string line = raw.substr(pos, lineEnd - pos);
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        present[toLowerCopy(trimCopy(line.substr(0, colon)))] = trimCopy(
          line.substr(colon + 1));
      }
      pos = lineEnd + 2;
    }
    auto it = present.find("x-amz-date");
    if (it == present.end()) {
      return std::nullopt;
    }
    amzDate = it->second;
  }
  std::string payloadHash = sha256Hex(body);
  std::string scope = date + "/" + region_ + "/hipobj/aws4_request";
  std::string canonicalRequest = method + "\n" + uri + "\n" + std::string() +
                                 "\n" + canonicalHeaderLines + "\n" +
                                 signedList + "\n" + payloadHash;
  std::string toSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" +
                       sha256Hex(canonicalRequest);
  std::string kDate = hmacSha256("AWS4" + secretKey_, date.substr(0, 8));
  std::string kRegion = hmacSha256(kDate, region_);
  std::string kService = hmacSha256(kRegion, "hipobj");
  std::string kSigning = hmacSha256(kService, "aws4_request");
  std::string signature = hex(reinterpret_cast<const unsigned char*>(
                                hmacSha256(kSigning, toSign).data()),
                              32);
  if (signature != providedSig) {
    return std::nullopt;
  }
  VerifiedCredential cred;
  cred.accessKey = key;
  cred.signedHeaders = signedList;
  return cred;
}
} // namespace v2
} // namespace hipObj
