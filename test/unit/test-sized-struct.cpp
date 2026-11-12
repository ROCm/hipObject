/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The public structs that start with structSize, and readSizedStruct() in
 * src/sized-struct.h, which reads them (see "Struct Sizes" in hipobj.h).
 * test-api-args.cpp checks that the public functions use it. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "hipobj.h"
#include "sized-struct.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

// ---- The public structs ----------------------------------------------

/* hipObjConfig_t and hipObjOps_t as they are in hipObject 0.1.0, the first
 * version with structSize. An application compiled against that version
 * passes structs laid out like these, so these must never change, and
 * the public structs can only add fields after them. */

struct ConfigV0_1_0 {
    size_t      structSize;
    uint32_t    flags;
    int         gpuDevice;
    const char *nicHint;
};

struct OpsV0_1_0 {
    size_t structSize;
    int (*sendRequest)(void *ctx, const char *token, size_t tokenLen);
    int (*recvReply)(void *ctx, char *reply, size_t *replyLen);
};

/* Each field of the 0.1.0 structs is where the public struct has it, with
 * the same type. These fail the build, so a change that moves, removes,
 * or retypes a field can't pass CI. */

static_assert(offsetof(hipObjConfig_t, structSize) == offsetof(ConfigV0_1_0, structSize));
static_assert(offsetof(hipObjConfig_t, flags) == offsetof(ConfigV0_1_0, flags));
static_assert(offsetof(hipObjConfig_t, gpuDevice) == offsetof(ConfigV0_1_0, gpuDevice));
static_assert(offsetof(hipObjConfig_t, nicHint) == offsetof(ConfigV0_1_0, nicHint));
static_assert(std::is_same_v<decltype(hipObjConfig_t::structSize), decltype(ConfigV0_1_0::structSize)>);
static_assert(std::is_same_v<decltype(hipObjConfig_t::flags), decltype(ConfigV0_1_0::flags)>);
static_assert(std::is_same_v<decltype(hipObjConfig_t::gpuDevice), decltype(ConfigV0_1_0::gpuDevice)>);
static_assert(std::is_same_v<decltype(hipObjConfig_t::nicHint), decltype(ConfigV0_1_0::nicHint)>);

static_assert(offsetof(hipObjOps_t, structSize) == offsetof(OpsV0_1_0, structSize));
static_assert(offsetof(hipObjOps_t, sendRequest) == offsetof(OpsV0_1_0, sendRequest));
static_assert(offsetof(hipObjOps_t, recvReply) == offsetof(OpsV0_1_0, recvReply));
static_assert(std::is_same_v<decltype(hipObjOps_t::structSize), decltype(OpsV0_1_0::structSize)>);
static_assert(std::is_same_v<decltype(hipObjOps_t::sendRequest), decltype(OpsV0_1_0::sendRequest)>);
static_assert(std::is_same_v<decltype(hipObjOps_t::recvReply), decltype(OpsV0_1_0::recvReply)>);

/* hipObject accepts a struct from 0.1.0 */
static_assert(hipObj::kConfigMinSize == sizeof(ConfigV0_1_0));
static_assert(hipObj::kOpsMinSize == sizeof(OpsV0_1_0));

/* Room for the structs to grow */
static_assert(sizeof(hipObjConfig_t) <= hipObj::kMaxStructSize);
static_assert(sizeof(hipObjOps_t) <= hipObj::kMaxStructSize);

// ---- readSizedStruct() -----------------------------------------------

/* A struct whose first version with structSize had only first, and whose
 * current version added second */
struct Current {
    size_t   structSize;
    uint64_t first;
    uint64_t second;
};

struct First {
    size_t   structSize;
    uint64_t first;
};

constexpr size_t kFirstSize = sizeof(First);

std::optional<Current>
readCurrent(const void *in)
{
    return hipObj::readSizedStruct<Current, kFirstSize>(in);
}

/* A Current from a newer version, with Extra bytes of fields that
 * Current doesn't have */
template <size_t Extra> struct Newer {
    Newer()
    {
        static_assert(sizeof(Newer) == sizeof(Current) + Extra, "Newer has no padding");
    }

    size_t                       structSize = sizeof(Newer);
    uint64_t                     first      = 1;
    uint64_t                     second     = 2;
    std::array<std::byte, Extra> newFields{};
};

TEST(SizedStruct, RejectsNull)
{
    EXPECT_FALSE(readCurrent(nullptr));
}

TEST(SizedStruct, CopiesTheCurrentVersion)
{
    const Current in{.structSize = sizeof(Current), .first = 1, .second = 2};
    const auto    copy = readCurrent(&in);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->structSize, sizeof(Current));
    EXPECT_EQ(copy->first, 1U);
    EXPECT_EQ(copy->second, 2U);
}

/* Only the first version's bytes are read, which AddressSanitizer checks,
 * and the field it doesn't have is zero */
TEST(SizedStruct, ZeroesFieldsAnOlderVersionDoesNotHave)
{
    const First in{.structSize = sizeof(First), .first = 1};
    const auto  copy = readCurrent(&in);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->structSize, sizeof(First));
    EXPECT_EQ(copy->first, 1U);
    EXPECT_EQ(copy->second, 0U);
}

TEST(SizedStruct, RejectsSizeBelowTheFirstVersion)
{
    for (const size_t structSize : {size_t{0}, size_t{1}, sizeof(size_t), kFirstSize - 1}) {
        const Current in{.structSize = structSize, .first = 1, .second = 2};
        EXPECT_FALSE(readCurrent(&in)) << "structSize " << structSize;
    }
}

TEST(SizedStruct, RejectsSizeAboveTheLimit)
{
    for (const size_t structSize : {hipObj::kMaxStructSize + 1, std::numeric_limits<size_t>::max()}) {
        const Current in{.structSize = structSize, .first = 1, .second = 2};
        EXPECT_FALSE(readCurrent(&in)) << "structSize " << structSize;
    }
}

TEST(SizedStruct, AcceptsANewerVersionWithNewFieldsZero)
{
    const Newer<8> newer;
    auto           copy = readCurrent(&newer);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->structSize, sizeof(newer));
    EXPECT_EQ(copy->first, 1U);
    EXPECT_EQ(copy->second, 2U);

    const Newer<hipObj::kMaxStructSize - sizeof(Current)> largest;
    copy = readCurrent(&largest);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->structSize, hipObj::kMaxStructSize);
    EXPECT_EQ(copy->second, 2U);
}

TEST(SizedStruct, RejectsANewerVersionWithANewFieldSet)
{
    Newer<8> newer;
    newer.newFields.back() = std::byte{1};
    EXPECT_FALSE(readCurrent(&newer));

    /* Every byte after Current's fields is checked */
    constexpr size_t kExtra = hipObj::kMaxStructSize - sizeof(Current);
    for (const size_t index : {size_t{0}, kExtra / 2, kExtra - 1}) {
        Newer<kExtra> largest;
        largest.newFields.at(index) = std::byte{0x80};
        EXPECT_FALSE(readCurrent(&largest)) << "byte " << index;
    }
}

} // namespace
