/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * The owning pointers whose deleters hold the interface they release
 * through (ibv-ptr.h, HostBufPtr, HipEventPtr), and own(). Each fake
 * records its calls in a FakeDevice of its own, so a deleter that released
 * through anything but its own interface would fail these tests.
 */

#include <memory>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "buffer.h"
#include "fake-device.h"
#include "hip-seam.h"
#include "hipobj-warnings.h"
#include "ibv-ptr.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* A pointer can't be built without the interface its deleter releases
 * through, either empty or from a raw pointer alone */
template <typename Ptr>
constexpr bool kNeedsDeleter =
    !std::is_default_constructible_v<Ptr> && !std::is_constructible_v<Ptr, typename Ptr::pointer>;
static_assert(kNeedsDeleter<hipObj::IbvContextPtr>);
static_assert(kNeedsDeleter<hipObj::IbvPdPtr>);
static_assert(kNeedsDeleter<hipObj::IbvCqPtr>);
static_assert(kNeedsDeleter<hipObj::IbvQpPtr>);
static_assert(kNeedsDeleter<hipObj::IbvMrPtr>);
static_assert(kNeedsDeleter<hipObj::IbvDeviceListPtr>);
static_assert(kNeedsDeleter<hipObj::HostBufPtr>);
static_assert(kNeedsDeleter<hipObj::HipEventPtr>);

/* Each owning verbs pointer releases its object through the Verbs it was
 * created with */
TEST(OwningPtrTest, VerbsPointersReleaseThroughTheirVerbs)
{
    hipObjTest::FakeDevice      device{};
    const hipObjTest::FakeVerbs verbs(device);
    {
        int        numDevices = 0;
        const auto list = hipObj::own<hipObj::IbvDeviceListPtr>(verbs, verbs.get_device_list(&numDevices));
        const auto ctx  = hipObj::own<hipObj::IbvContextPtr>(verbs, verbs.open_device(nullptr));
        const auto pd   = hipObj::own<hipObj::IbvPdPtr>(verbs, verbs.alloc_pd(ctx.get()));
        const auto cq =
            hipObj::own<hipObj::IbvCqPtr>(verbs, verbs.create_cq(ctx.get(), 1, nullptr, nullptr, 0));
        const auto qp   = hipObj::own<hipObj::IbvQpPtr>(verbs, verbs.create_qp(pd.get(), nullptr));
        char       byte = 0;
        const auto mr   = hipObj::own<hipObj::IbvMrPtr>(verbs, verbs.reg_mr(pd.get(), &byte, 1, 0));
        ASSERT_TRUE(list && ctx && pd && cq && qp && mr);
        EXPECT_EQ(device.log.destroyQp, 0);
    }
    EXPECT_EQ(device.log.freeDeviceList, 1);
    EXPECT_EQ(device.log.closeDevice, 1);
    EXPECT_EQ(device.log.deallocPd, 1);
    EXPECT_EQ(device.log.destroyCq, 1);
    EXPECT_EQ(device.log.destroyQp, 1);
    EXPECT_EQ(device.log.deregMr, 1);
}

/* Pointers from two Verbs each release through their own */
TEST(OwningPtrTest, PointersFromTwoVerbsStaySeparate)
{
    hipObjTest::FakeDevice      first{};
    const hipObjTest::FakeVerbs firstVerbs(first);
    hipObjTest::FakeDevice      second{};
    const hipObjTest::FakeVerbs secondVerbs(second);
    {
        const auto a = hipObj::own<hipObj::IbvQpPtr>(firstVerbs, firstVerbs.create_qp(nullptr, nullptr));
        const auto b = hipObj::own<hipObj::IbvQpPtr>(secondVerbs, secondVerbs.create_qp(nullptr, nullptr));
        const auto c = hipObj::own<hipObj::IbvQpPtr>(secondVerbs, secondVerbs.create_qp(nullptr, nullptr));
        ASSERT_TRUE(a && b && c);
    }
    EXPECT_EQ(first.log.destroyQp, 1);
    EXPECT_EQ(second.log.destroyQp, 2);
}

/* Each owning HIP pointer releases its object through the runtime it was
 * created with */
TEST(OwningPtrTest, HipPointersReleaseThroughTheirRuntime)
{
    hipObjTest::FakeDevice    device{};
    const hipObjTest::FakeHip runtime(device);
    void                     *rawHostBuf = nullptr;
    {
        ASSERT_EQ(runtime.hostMalloc(&rawHostBuf, 16, hipHostMallocDefault), hipSuccess);
        const auto hostBuf = hipObj::own<hipObj::HostBufPtr>(runtime, rawHostBuf);

        hipEvent_t rawEvent = nullptr;
        ASSERT_EQ(runtime.eventCreate(&rawEvent), hipSuccess);
        const auto event = hipObj::own<hipObj::HipEventPtr>(runtime, rawEvent);

        EXPECT_EQ(device.log.hostFree, 0);
    }
    EXPECT_EQ(device.log.hostFree, 1);
    EXPECT_EQ(device.lastHostFree, rawHostBuf);
    EXPECT_EQ(device.log.eventDestroy, 1);
}

/* A pointer's deleter moves with its object, so moving one into another
 * pointer releases the target's object through the target's Verbs, and
 * the moved object through the Verbs it came from */
TEST(OwningPtrTest, MovingMovesTheDeleter)
{
    hipObjTest::FakeDevice      first{};
    const hipObjTest::FakeVerbs firstVerbs(first);
    hipObjTest::FakeDevice      second{};
    const hipObjTest::FakeVerbs secondVerbs(second);
    {
        auto target = hipObj::own<hipObj::IbvQpPtr>(firstVerbs, firstVerbs.create_qp(nullptr, nullptr));
        {
            auto source = hipObj::own<hipObj::IbvQpPtr>(secondVerbs, secondVerbs.create_qp(nullptr, nullptr));
            ASSERT_TRUE(target && source);
            target = std::move(source);
        }
        EXPECT_EQ(first.log.destroyQp, 1);
        EXPECT_EQ(target.get_deleter().verbs, &secondVerbs);
        EXPECT_EQ(second.log.destroyQp, 0);
    }
    EXPECT_EQ(second.log.destroyQp, 1);
}

/* own() of a null pointer is empty, and releases nothing */
TEST(OwningPtrTest, OwningNullReleasesNothing)
{
    hipObjTest::FakeDevice      device{};
    const hipObjTest::FakeVerbs verbs(device);
    {
        const auto qp = hipObj::own<hipObj::IbvQpPtr>(verbs, nullptr);
        EXPECT_FALSE(qp);
        EXPECT_EQ(qp.get_deleter().verbs, &verbs);
    }
    EXPECT_EQ(device.log.destroyQp, 0);
}

} // namespace
