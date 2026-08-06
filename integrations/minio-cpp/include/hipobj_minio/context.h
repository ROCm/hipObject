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
};

} // namespace hipobj::minio
