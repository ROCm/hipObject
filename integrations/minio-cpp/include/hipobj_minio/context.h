/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

#include <miniocpp/baseclient.h>
#include <miniocpp/credentials.h>
#include <miniocpp/http.h>

namespace hipobj::minio {

/* The enclosing namespace name shadows the minio-cpp root namespace
 * for qualified lookups inside hipobj::minio; rebind it explicitly. */
namespace minio = ::minio;

struct S3RdmaContext {
  minio::creds::Provider* provider = nullptr;
  std::string bucket;
  std::string object;
  minio::s3::BaseUrl url;
  std::string region;
  std::string uploadId;
  unsigned int partNumber = 0;
  std::string checksum;
  std::string etag;
  /* Last successful provider fetch for this transfer. Empty
   * expiration never expires; a timed credential is reused until
   * Credentials::operator bool() reports it spent. */
  minio::creds::Credentials cachedCreds{};
  bool haveCachedCreds = false;
};

} // namespace hipobj::minio
