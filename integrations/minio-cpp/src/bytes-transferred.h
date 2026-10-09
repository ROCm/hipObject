/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Reading the x-amz-rdma-bytes-transferred response header. This has no
 * minio-cpp dependency, so the unit tests can include it. */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "hipobj-parse.h"

namespace hipobj::minio {

/* Parses the value of an x-amz-rdma-bytes-transferred header. value is
 * std::nullopt if the response didn't have one, which gives ifAbsent. A
 * value that's present has to be a plain decimal count that fits in 64
 * bits; anything else, including an empty value, gives std::nullopt. */
inline std::optional<uint64_t>
parseBytesTransferred(const std::optional<std::string> &value, uint64_t ifAbsent)
{
    if (!value) {
        return ifAbsent;
    }
    return hipObj::parseNumber<uint64_t>(*value);
}

} // namespace hipobj::minio
