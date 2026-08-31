/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* AWS SigV4 verification and signing for the v2 control protocol.
 *
 * The server verifies each request independently against its
 * configured key pair and reports the authenticated access key so
 * sessions can compare credential identity between PREPARE and
 * READY/CANCEL. The signing helper exists so test harnesses
 * produce valid requests with the same canonicalization. */

#pragma once

#include <map>
#include <optional>
#include <string>

namespace hipObj {
namespace v2 {

struct VerifiedCredential {
  std::string accessKey;
  std::string signedHeaders; /* "host;x-amz-date;..." */
};

class SigV4Verifier {
public:
  virtual ~SigV4Verifier() = default;

  /* Verifies the Authorization header in rawHeaderBlock against
   * method/uri (the request target) and body. Returns the parsed
   * credential identity on success. */
  virtual std::optional<VerifiedCredential> verify(
    const std::string& method, const std::string& uri,
    const std::string& rawHeaderBlock, const std::string& body) = 0;
};

/* Single-key verifier used by the reference server and tests. */
class BuiltinVerifier : public SigV4Verifier {
public:
  BuiltinVerifier(std::string accessKey, std::string secretKey,
                  std::string region);

  std::optional<VerifiedCredential> verify(const std::string& method,
                                           const std::string& uri,
                                           const std::string& rawHeaderBlock,
                                           const std::string& body) override;

  /* Exposed for the signing side and known-answer tests. */
  std::string sign(const std::string& method, const std::string& uri,
                   const std::map<std::string, std::string>& headers,
                   const std::string& body, const std::string& amzDate) const;

  const std::string& accessKey() const {
    return accessKey_;
  }

private:
  std::string accessKey_;
  std::string secretKey_;
  std::string region_;
};

/* Extracts the raw Authorization value from a header block
 * (shared with the request parsers). */
std::string extractAuthorization(const std::string& rawHeaders);

} // namespace v2
} // namespace hipObj
