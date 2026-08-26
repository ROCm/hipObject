/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_backend.h"

#include <cstdio>
#include <cstring>

namespace hipObj {
namespace v2 {

namespace {

uint8_t patternByte(const std::string& target, size_t i) {
  uint32_t h = 2166136261u;
  for (char c : target) {
    h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  }
  return static_cast<uint8_t>((h + static_cast<uint32_t>(i)) & 0xff);
}

std::string pseudoEtag(const void* data, size_t len) {
  /* Simple deterministic digest - not a real MD5; format matches
   * the 32-hex S3 ETag shape for response parsing tests. */
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t a = 1469598103934665603ull;
  uint64_t b = 7;
  for (size_t i = 0; i < len; ++i) {
    a = (a * 31 + bytes[i] + i) ^ (b << 3);
    b = b * 29 + bytes[i];
  }
  char out[33];
  std::snprintf(out, sizeof(out), "%016zx%016zx", static_cast<size_t>(a),
                static_cast<size_t>(b));
  return out;
}

} // namespace

void MemoryBackend::seed(const std::string& target, size_t size) {
  std::lock_guard<std::mutex> guard(mtx_);
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = patternByte(target, i);
  }
  objects_[target] = std::move(data);
}

bool MemoryBackend::has(const std::string& target) const {
  std::lock_guard<std::mutex> guard(mtx_);
  return objects_.count(target) != 0;
}

bool MemoryBackend::read(const std::string& target, uint64_t offset, size_t len,
                         void* dst) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = objects_.find(target);
  if (it == objects_.end() || offset + len > it->second.size()) {
    return false;
  }
  std::memcpy(dst, it->second.data() + offset, len);
  return true;
}

std::string MemoryBackend::write(const std::string& target, const void* data,
                                 size_t len) {
  std::lock_guard<std::mutex> guard(mtx_);
  const auto* bytes = static_cast<const uint8_t*>(data);
  std::string etag = pseudoEtag(data, len);
  objects_[target] = std::vector<uint8_t>(bytes, bytes + len);
  return etag;
}

size_t MemoryBackend::objectCount() const {
  std::lock_guard<std::mutex> guard(mtx_);
  return objects_.size();
}

} // namespace v2
} // namespace hipObj
