/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* In-memory object store backing the v2 reference server. */

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hipObj {
namespace v2 {

class MemoryBackend {
public:
  /* Seeds an object from deterministic pattern bytes. */
  void seed(const std::string& target, size_t size);
  bool has(const std::string& target) const;

  /* Copies object bytes into the caller buffer (GET staging).
   * Returns false when the object is absent or too small. */
  bool read(const std::string& target, uint64_t offset, size_t len, void* dst);

  /* Stores PUT data; produces a 32-hex pseudo etag. */
  std::string write(const std::string& target, const void* data, size_t len);

  size_t objectCount() const;

private:
  mutable std::mutex mtx_;
  std::map<std::string, std::vector<uint8_t>> objects_;
};

} // namespace v2
} // namespace hipObj
