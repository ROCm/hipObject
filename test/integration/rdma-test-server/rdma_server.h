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

  // v2 protocol — PREPARE phase: connect QP to client, stage data for GET or
  // record transfer size for PUT. Returns a session ID in replyHeader and
  // encodes the server token so the client can complete the QP handshake.
  int prepareWriteToClient(const std::string& tokenHeader,
                           const std::vector<uint8_t>& data,
                           std::string& replyHeader, std::string& sessionId);
  int prepareReadFromClient(const std::string& tokenHeader, size_t size,
                            std::string& replyHeader, std::string& sessionId);

  // v2 protocol — READY phase: post the RDMA operation for a previously
  // prepared session and poll for completion.
  int completeWriteToClient(const std::string& sessionId,
                            std::vector<uint8_t>& data);
  int completeReadFromClient(const std::string& sessionId,
                             std::vector<uint8_t>& outData);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace hipobj::test
