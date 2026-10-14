/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hipObj {
class Verbs;
} // namespace hipObj

namespace hipobj::test {

struct TokenHeader;

class RdmaTestServer {
public:
    /* Sets up RDMA through verbs, which has to outlive the server. With
     * null verbs (libibverbs didn't load), the server isn't ready. */
    explicit RdmaTestServer(const hipObj::Verbs *verbs);
    ~RdmaTestServer();

    RdmaTestServer(const RdmaTestServer &)            = delete;
    RdmaTestServer &operator=(const RdmaTestServer &) = delete;
    RdmaTestServer(RdmaTestServer &&)                 = delete;
    RdmaTestServer &operator=(RdmaTestServer &&)      = delete;

    bool isReady() const;

    int rdmaWriteToClient(const TokenHeader &tokenHeader, const std::vector<uint8_t> &data,
                          std::string &replyHeader);

    int rdmaReadFromClient(const TokenHeader &tokenHeader, size_t size, std::vector<uint8_t> &data,
                           std::string &replyHeader);

private:
    /* Null when the server couldn't set up RDMA */
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

} // namespace hipobj::test
