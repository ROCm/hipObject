/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

    [[nodiscard]] bool isReady() const;

    /* What rdmaReadFromClient() read */
    struct ReadResult {
        std::vector<uint8_t> data{};
        std::string          replyHeader{}; /* The x-amz-rdma-reply value */
    };

    /* Writes data to the client's buffer. Returns the x-amz-rdma-reply
     * value, or std::nullopt, after a message, if the transfer failed. */
    [[nodiscard]] std::optional<std::string> rdmaWriteToClient(const TokenHeader          &tokenHeader,
                                                               const std::vector<uint8_t> &data);

    /* Reads size bytes from the client's buffer, or as many as the token
     * header gives if size is 0. Returns std::nullopt, after a message, if
     * the transfer failed. */
    [[nodiscard]] std::optional<ReadResult> rdmaReadFromClient(const TokenHeader &tokenHeader, size_t size);

private:
    /* Null when the server couldn't set up RDMA */
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

} // namespace hipobj::test
