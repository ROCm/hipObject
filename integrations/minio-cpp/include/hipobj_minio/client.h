/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <miniocpp/args.h>
#include <miniocpp/client.h>
#include <miniocpp/credentials.h>
#include <miniocpp/response.h>

namespace hipobj::minio {

class Client {
public:
  Client(minio::s3::BaseUrl base_url, minio::creds::Provider* provider);

  minio::s3::PutObjectResponse PutObject(minio::s3::PutObjectArgs args);
  minio::s3::GetObjectResponse GetObject(minio::s3::GetObjectArgs args);

  bool RdmaAvailable() const;

private:
  minio::s3::BaseUrl base_url_;
  minio::creds::Provider* provider_ = nullptr;
  minio::s3::Client s3_client_;
};

} // namespace hipobj::minio
