/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Exceptions must not cross the C API. Each public function that calls into
 * a seam or one of the caller's callbacks is run with one that throws, and
 * must return hipObjInternalError instead, against the fake device in
 * fake-device.h. hipObjInit() always uses the system platform, so
 * initialization is tested through initWith() instead, which must leave
 * nothing initialized or open when it throws.
 *
 * Only the seams that acquire something throw here. A seam that releases
 * something (dereg_mr, destroy_qp, ...) runs in a destructor, where an
 * exception terminates the program, so hipObjBufDeregister() and
 * hipObjShutdown() can't be tested this way.
 */

#include <cstddef>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "fake-device.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "state.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::FakeHip;
using hipObjTest::FakeVerbs;
using hipObjTest::makeOps;

void *const kDevBuf = hipObjTest::fakeDevBuf();

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

/* A standard exception, or something else entirely */
enum class Thrown { StdException, Int };

Thrown g_thrown = Thrown::StdException;

[[noreturn]] void
throwIt()
{
    if (g_thrown == Thrown::StdException) {
        throw std::runtime_error("thrown by a fake");
    }
    /* Not a std::exception, to test the C API's catch (...). clang-tidy
     * before LLVM 23 names the check hicpp-exception-baseclass. */
    // NOLINTNEXTLINE(bugprone-std-exception-baseclass,hicpp-exception-baseclass)
    throw 42;
}

/* The fake HIP runtime, with the call that starts hipObjInit() throwing */
class ThrowingGetDeviceHip : public hipObjTest::FakeHip {
public:
    using FakeHip::FakeHip;

    hipError_t getDevice(int *) const override
    {
        throwIt();
    }
};

/* The fake HIP runtime, with a staging copy throwing */
class ThrowingMemcpyHip : public hipObjTest::FakeHip {
public:
    using FakeHip::FakeHip;

    hipError_t memcpyAsync(void *, const void *, size_t, hipMemcpyKind, hipStream_t) const override
    {
        throwIt();
    }
};

/* The fake verbs, with creating a QP throwing */
class ThrowingCreateQpVerbs : public hipObjTest::FakeVerbs {
public:
    using FakeVerbs::FakeVerbs;

    struct ibv_qp *create_qp(struct ibv_pd *, struct ibv_qp_init_attr *) const override
    {
        throwIt();
    }
};

/* The fake verbs, with registering memory throwing */
class ThrowingRegMrVerbs : public hipObjTest::FakeVerbs {
public:
    using FakeVerbs::FakeVerbs;

    struct ibv_mr *reg_mr(struct ibv_pd *, void *, size_t, int) const override
    {
        throwIt();
    }
};

int
throwingSendRequest(void *, const char *, size_t)
{
    throwIt();
}

int
throwingRecvReply(void *, char *, size_t *)
{
    throwIt();
}

class ApiExceptionTest : public hipObjTest::FakeDeviceTest, public ::testing::WithParamInterface<Thrown> {
protected:
    void SetUp() override
    {
        FakeDeviceTest::SetUp();
        g_thrown = GetParam();
    }

    /* Initializes the library with a FakePlatform over Hip and Verbs,
     * which the instance calls from then on */
    template <typename Hip, typename Verbs> static hipObjError_t initThrough()
    {
        const auto fakes = std::make_shared<hipObjTest::FakePlatform<Hip, Verbs>>(fake());
        return hipObjTest::initWithPlatform(makeConfig(), fakes->platform());
    }
};

TEST_P(ApiExceptionTest, Init)
{
    EXPECT_ANY_THROW((void)(initThrough<ThrowingGetDeviceHip, FakeVerbs>()));
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
}

TEST_P(ApiExceptionTest, InitAfterOpeningDevice)
{
    EXPECT_ANY_THROW((void)(initThrough<FakeHip, ThrowingCreateQpVerbs>()));
    EXPECT_EQ(hipObj::currentInstance(), nullptr);

    /* The device, PD, and CQ opened before the throw are released */
    const hipObjTest::CallLog &log = fake().log;
    EXPECT_EQ(log.openDevice, 1);
    EXPECT_EQ(log.closeDevice, 1);
    EXPECT_EQ(log.allocPd, 1);
    EXPECT_EQ(log.deallocPd, 1);
    EXPECT_EQ(log.createCq, 1);
    EXPECT_EQ(log.destroyCq, 1);

    /* The library still works */
    ASSERT_NO_FATAL_FAILURE(init());
}

TEST_P(ApiExceptionTest, BufRegister)
{
    ASSERT_EQ((initThrough<FakeHip, ThrowingRegMrVerbs>().opError), hipObjSuccess);
    EXPECT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjInternalError);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjBufNotRegistered);
}

TEST_P(ApiExceptionTest, BufRegisterHost)
{
    ASSERT_EQ((initThrough<FakeHip, ThrowingRegMrVerbs>().opError), hipObjSuccess);
    EXPECT_EQ(hipObjBufRegisterHost(kDevBuf, kBufSize).opError, hipObjInternalError);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjBufNotRegistered);
}

TEST_P(ApiExceptionTest, GetAndPut)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    ops.sendRequest = &throwingSendRequest;
    EXPECT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjInternalError);
    EXPECT_EQ(hipObjPut(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjInternalError);

    ops           = makeOps();
    ops.recvReply = &throwingRecvReply;
    EXPECT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjInternalError);
    EXPECT_EQ(hipObjPut(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjInternalError);

    /* The library still works */
    ops = makeOps();
    EXPECT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjSuccess);
}

TEST_P(ApiExceptionTest, BufSync)
{
    ASSERT_EQ((initThrough<ThrowingMemcpyHip, FakeVerbs>().opError), hipObjSuccess);
    /* Registered through a staging buffer, so a sync copies */
    fake().fail.registerAddr = kDevBuf;
    ASSERT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize, 0, HIPOBJ_SYNC_TO_DEVICE).opError, hipObjInternalError);
}

INSTANTIATE_TEST_SUITE_P(ApiException, ApiExceptionTest, ::testing::Values(Thrown::StdException, Thrown::Int),
                         [](const ::testing::TestParamInfo<Thrown> &paramInfo) {
                             return paramInfo.param == Thrown::StdException ? "StdException" : "Int";
                         });

} // namespace
