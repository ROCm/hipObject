/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj_minio/client.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>

#include <hipobj.h>
#include <miniocpp/baseclient.h>
#include <miniocpp/error.h>
#include <miniocpp/http.h>

#include "hipobj_minio/rdma.h"

namespace hipobj::minio {

using namespace ::minio;

namespace {

class HipObjRuntime {
public:
  static HipObjRuntime& Instance() {
    static HipObjRuntime inst;
    return inst;
  }

  hipObjError_t EnsureInit(minio::s3::BaseUrl base_url,
                           minio::creds::Provider* provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = base_url.host + ":" + base_url.region;
    if (initialized_ && key == active_key_) {
      return HIPOBJ_SUCCESS;
    }
    if (initialized_) {
      hipObjShutdown();
      initialized_ = false;
    }
    hipObjConfig_t cfg{};
    endpoint_storage_ = (base_url.https ? "https://" : "http://") +
                        base_url.host;
    cfg.endpoint = endpoint_storage_.c_str();
    cfg.region = base_url.region.c_str();
    cfg.gpuDevice = -1;
    hipObjError_t err = hipObjInit(&cfg);
    if (err.opError == hipObjSuccess) {
      initialized_ = true;
      active_key_ = key;
    }
    return err;
  }

  bool IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
  }

private:
  HipObjRuntime() = default;

  mutable std::mutex mutex_;
  bool initialized_ = false;
  std::string active_key_;
  std::string endpoint_storage_;
};

struct ScopedBufRegistration {
  void* ptr = nullptr;
  bool owned = false;
  ScopedBufRegistration(void* p, bool own) : ptr(p), owned(own) {
  }
  ~ScopedBufRegistration() {
    if (owned && ptr) {
      hipObjBufDeregister(ptr);
    }
  }
};

} // namespace

Client::Client(minio::s3::BaseUrl base_url, minio::creds::Provider* provider)
  : base_url_(base_url), provider_(provider), s3_client_(base_url, provider) {
}

bool Client::RdmaAvailable() const {
  return HipObjRuntime::Instance().IsReady();
}

minio::s3::PutObjectResponse Client::PutObject(minio::s3::PutObjectArgs args) {
  if (minio::error::Error err = args.Validate()) {
    return minio::s3::PutObjectResponse(err);
  }

  if (args.buf == nullptr) {
    return s3_client_.PutObject(args);
  }

  const size_t size = *args.size;

  hipObjError_t init_err = HipObjRuntime::Instance().EnsureInit(base_url_,
                                                                provider_);
  if (init_err.opError != hipObjSuccess) {
    // RDMA not available (no NIC or no GPU topology) — fall back to HTTP PUT.
    // Copy the buffer into a std::stringstream so minio-cpp can seek in it.
    const char* src = static_cast<const char*>(args.buf);
    std::string content(src, size);
    std::stringstream ss(content, std::ios_base::in | std::ios_base::binary);
    minio::s3::PutObjectArgs http_args = args;
    http_args.stream = &ss;
    http_args.buf = nullptr;
    http_args.size = std::nullopt;
    http_args.object_size = static_cast<long>(size);
    http_args.part_size = 0;
    return s3_client_.PutObject(http_args);
  }

  bool registered_here = false;
  hipObjError_t reg_err = hipObjBufRegister(args.buf, size);
  if (reg_err.opError == hipObjSuccess) {
    registered_here = true;
  } else if (reg_err.opError != hipObjBufAlreadyRegistered) {
    return minio::s3::PutObjectResponse(
      minio::error::Error("hipObjBufRegister failed: " +
                          std::string(hipObjGetErrorString(reg_err.opError))));
  }
  ScopedBufRegistration reg_guard(args.buf, registered_here);

  std::string region;
  if (minio::s3::GetRegionResponse resp = s3_client_.GetRegion(args.bucket,
                                                               args.region)) {
    region = resp.region;
  } else {
    return minio::s3::PutObjectResponse(resp);
  }

  S3RdmaContext put_ctx{
    .provider = provider_,
    .bucket = args.bucket,
    .object = args.object,
    .url = base_url_,
    .region = region,
  };

  ssize_t ret = rdmaPutWithRetry(&put_ctx, args.buf, size);
  if (ret > 0) {
    minio::s3::PutObjectResponse resp;
    resp.etag = put_ctx.etag;
    return resp;
  }

  minio::s3::PutObjectArgs http_args = args;
  std::stringstream ss(std::ios_base::in | std::ios_base::out |
                       std::ios_base::binary);
  ss.rdbuf()->pubsetbuf(args.buf, static_cast<std::streamsize>(size));
  http_args.stream = &ss;
  http_args.buf = nullptr;
  http_args.size = std::nullopt;
  http_args.object_size = static_cast<long>(size);
  return s3_client_.PutObject(http_args);
}

minio::s3::GetObjectResponse Client::GetObject(minio::s3::GetObjectArgs args) {
  if (minio::error::Error err = args.Validate()) {
    return minio::s3::GetObjectResponse(err);
  }

  if (args.buf == nullptr) {
    return s3_client_.GetObject(args);
  }

  const size_t size = *args.size;

  hipObjError_t init_err = HipObjRuntime::Instance().EnsureInit(base_url_,
                                                                provider_);
  if (init_err.opError != hipObjSuccess) {
    // RDMA not available — fall back to HTTP GET.
    // Accumulate response into a string, then memcpy to the caller's buffer.
    std::string received;
    received.reserve(size);
    minio::s3::GetObjectArgs http_args = args;
    http_args.datafunc = [&](minio::http::DataFunctionArgs chunk) -> bool {
      received.append(chunk.datachunk);
      return true;
    };
    http_args.buf = nullptr;
    http_args.size = std::nullopt;
    minio::s3::GetObjectResponse resp = s3_client_.GetObject(http_args);
    if (resp) {
      size_t copy_len = std::min(received.size(), size);
      std::memcpy(args.buf, received.data(), copy_len);
    }
    return resp;
  }

  bool registered_here = false;
  hipObjError_t reg_err = hipObjBufRegister(args.buf, size);
  if (reg_err.opError == hipObjSuccess) {
    registered_here = true;
  } else if (reg_err.opError != hipObjBufAlreadyRegistered) {
    return minio::s3::GetObjectResponse(
      minio::error::Error("hipObjBufRegister failed: " +
                          std::string(hipObjGetErrorString(reg_err.opError))));
  }
  ScopedBufRegistration reg_guard(args.buf, registered_here);

  std::string region;
  if (minio::s3::GetRegionResponse resp = s3_client_.GetRegion(args.bucket,
                                                               args.region)) {
    region = resp.region;
  } else {
    return minio::s3::GetObjectResponse(resp);
  }

  S3RdmaContext get_ctx{
    .provider = provider_,
    .bucket = args.bucket,
    .object = args.object,
    .url = base_url_,
    .region = region,
  };

  ssize_t ret = rdmaGetWithRetry(&get_ctx, args.buf, size);
  if (ret > 0) {
    minio::s3::GetObjectResponse resp;
    resp.etag = get_ctx.etag;
    return resp;
  }

  minio::s3::GetObjectArgs http_args = args;
  char* dst = static_cast<char*>(args.buf);
  size_t offset = 0;
  http_args.datafunc = [&](minio::http::DataFunctionArgs chunk) -> bool {
    size_t len = chunk.datachunk.size();
    if (offset + len > size)
      len = size - offset;
    std::memcpy(dst + offset, chunk.datachunk.data(), len);
    offset += len;
    return true;
  };
  http_args.buf = nullptr;
  http_args.size = std::nullopt;
  return s3_client_.GetObject(http_args);
}

} // namespace hipobj::minio
