/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The views in shared/hipobj-span.h, and hipObj::deviceSpan() in
 * ibv-ptr.h */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "fake-device.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-ptr.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

TEST(Span, SpanFromPointerViewsTheObjects)
{
    std::array<int, 4>   values{{1, 2, 3, 4}};
    const std::span<int> view = hipObj::spanFromPointer(values.data(), 3);
    EXPECT_EQ(view.data(), values.data());
    ASSERT_EQ(view.size(), 3U);
    view[2] = 30;
    EXPECT_EQ(values[2], 30);
}

TEST(Span, CStringViewStopsAtTheNul)
{
    const std::string text = "token";
    EXPECT_EQ(hipObj::cStringView(text.c_str(), 100), "token");
    EXPECT_EQ(hipObj::cStringView("", 100), "");
    /* The NUL itself is within the limit */
    EXPECT_EQ(hipObj::cStringView(text.c_str(), text.size() + 1), "token");
}

TEST(Span, CStringViewCutsALongString)
{
    /* No NUL in the first maxLen bytes, so it scans no further */
    const std::array<char, 4> unterminated{{'a', 'b', 'c', 'd'}};
    EXPECT_EQ(hipObj::cStringView(unterminated.data(), unterminated.size()), "abcd");
    EXPECT_EQ(hipObj::cStringView("token", 3), "tok");
    EXPECT_EQ(hipObj::cStringView("token", 0), "");
}

TEST(Span, CStringViewOfAnArrayStopsAtTheNulOrTheEnd)
{
    const std::array<char, 8> terminated{{'m', 'l', 'x', '5', '\0', 'x', 'x', 'x'}};
    EXPECT_EQ(hipObj::cStringView(terminated), "mlx5");

    const std::array<char, 4> unterminated{{'a', 'b', 'c', 'd'}};
    EXPECT_EQ(hipObj::cStringView(unterminated), "abcd");
}

TEST(Span, AsStringViewViewsTheSameCharacters)
{
    const std::array<char, 5> chars{{'h', 'e', 'l', 'l', 'o'}};
    const std::string_view    text = hipObj::asStringView(std::span(chars).first(4));
    EXPECT_EQ(text, "hell");
    EXPECT_EQ(text.data(), chars.data());
}

TEST(Span, ArgSpanViewsTheArguments)
{
    std::string           prog = "prog";
    std::string           arg  = "--flag";
    std::array<char *, 3> argv{{prog.data(), arg.data(), nullptr}};

    const std::span<char *> args = hipObj::argSpan(2, argv.data());
    ASSERT_EQ(args.size(), 2U);
    EXPECT_EQ(std::string_view(args[1]), "--flag");

    EXPECT_TRUE(hipObj::argSpan(0, argv.data()).empty());
    EXPECT_TRUE(hipObj::argSpan(-1, argv.data()).empty());
}

/* Padding after c and after s, on every ABI hipObject builds for */
struct Padded {
    char     c;
    uint64_t u;
    uint16_t s;
};

TEST(Span, ZeroObjectZeroesThePadding)
{
    Padded padded{};
    std::ranges::fill(std::as_writable_bytes(std::span<Padded, 1>{&padded, 1}), std::byte{0xa5});
    hipObj::zeroObject(padded);
    const std::span<const std::byte, sizeof(Padded)> bytes = std::as_bytes(std::span<Padded, 1>{&padded, 1});
    EXPECT_TRUE(std::ranges::all_of(bytes, [](std::byte b) noexcept { return b == std::byte{0}; }));
}

TEST(Span, ZeroedObjectZeroesThePadding)
{
    /* Built in memory that isn't zero to begin with. The result
     * initializes the new Padded directly, without a copy, as it does a
     * variable declared with it. */
    alignas(Padded) std::array<std::byte, sizeof(Padded)> storage{};
    std::ranges::fill(storage, std::byte{0xa5});
    ::new (static_cast<void *>(storage.data())) Padded(hipObj::zeroedObject<Padded>());
    EXPECT_TRUE(std::ranges::all_of(storage, [](std::byte b) noexcept { return b == std::byte{0}; }));
}

TEST(Span, ZeroedObjectZeroesEnumsWithoutAZeroEnumerator)
{
    /* ibv_mtu has no zero enumerator, and the member is zero anyway */
    const auto attr = hipObj::zeroedObject<ibv_qp_attr>();
    EXPECT_EQ(static_cast<int>(attr.path_mtu), 0);
    EXPECT_EQ(attr.qp_state, IBV_QPS_RESET);
}

TEST(Span, DeviceSpanViewsTheList)
{
    /* Two devices and the NULL that ends the list, as from
     * ibv_get_device_list(). The devices' values don't matter here. */
    std::array<ibv_device *, 3> devices{};

    /* The list is borrowed, so it's released rather than freed with
     * verbs */
    hipObjTest::FakeDevice      device{};
    const hipObjTest::FakeVerbs verbs(device);
    auto                        list = hipObj::own<hipObj::IbvDeviceListPtr>(verbs, devices.data());
    const auto                  view = hipObj::deviceSpan(list, 2);
    EXPECT_EQ(view.data(), devices.data());
    EXPECT_EQ(view.size(), 2U);
    EXPECT_TRUE(hipObj::deviceSpan(list, 0).empty());
    EXPECT_TRUE(hipObj::deviceSpan(list, -1).empty());
    hipObj::dropOwnership(list);

    EXPECT_TRUE(hipObj::deviceSpan(list, 2).empty());
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
