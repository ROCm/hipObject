/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace hipobj::test {

class RdmaTestServer {
public:
  RdmaTestServer();
  ~RdmaTestServer();

  RdmaTestServer(const RdmaTestServer&) = delete;
  RdmaTestServer& operator=(const RdmaTestServer&) = delete;

  bool isReady() const;

  int rdmaWriteToClient(const std::string& tokenHeader,
                        const std::vector<uint8_t>& data,
                        std::string& replyHeader);

  int rdmaReadFromClient(const std::string& tokenHeader, size_t size,
                         std::vector<uint8_t>& data,
                         std::string& replyHeader);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace hipobj::test
