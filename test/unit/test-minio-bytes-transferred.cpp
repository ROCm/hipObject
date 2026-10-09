/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The MinIO bridge's handling of the x-amz-rdma-bytes-transferred header
 * (integrations/minio-cpp/src/bytes-transferred.h). It doesn't need
 * minio-cpp, so it's tested here, where it's built whether or not the
 * bridge is. */

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "bytes-transferred.h"
#include "hipobj-warnings.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipobj::minio::parseBytesTransferred;

TEST(MinioBytesTransferred, MissingHeaderGivesDefault)
{
    EXPECT_EQ(parseBytesTransferred(std::nullopt, 0), 0U);
    EXPECT_EQ(parseBytesTransferred(std::nullopt, 4096), 4096U);
}

TEST(MinioBytesTransferred, ReadsCount)
{
    EXPECT_EQ(parseBytesTransferred(std::string("65536"), 0), 65536U);
    EXPECT_EQ(parseBytesTransferred(std::string("0"), 4096), 0U);
    EXPECT_EQ(parseBytesTransferred(std::string("18446744073709551615"), 0),
              std::numeric_limits<uint64_t>::max());
}

TEST(MinioBytesTransferred, RejectsMalformedCount)
{
    /* An empty value is malformed, not missing */
    for (const char *value :
         {"", " ", "12abc", "12 ", " 12", "+12", "-1", "0x10", "1.5", "18446744073709551616"}) {
        EXPECT_FALSE(parseBytesTransferred(std::string(value), 4096)) << "\"" << value << "\"";
    }
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
