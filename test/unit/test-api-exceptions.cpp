/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Exceptions must not cross the C API. Each public function that calls into
 * a seam or one of the caller's callbacks is run with one that throws, and
 * must return hipObjInternalError instead, against the fake device in
 * fake-device.h.
 *
 * Only the seams that acquire something throw here. A seam that releases
 * something (dereg_mr, destroy_qp, ...) runs in a destructor, where an
 * exception terminates the program, so hipObjBufDeregister() and
 * hipObjShutdown() can't be tested this way.
 */

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "fake-device.h"
#include "hip-seam.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "state.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
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
    throw 42;
}

hipError_t
throwingGetDevice(int *)
{
    throwIt();
}

hipError_t
throwingMemcpy(void *, const void *, size_t, hipMemcpyKind)
{
    throwIt();
}

struct ibv_mr *
throwingRegisterMr(struct ibv_pd *, void *, size_t, int)
{
    throwIt();
}

struct ibv_mr *
throwingRegisterMrIova2(struct ibv_pd *, void *, size_t, uintptr_t, int)
{
    throwIt();
}

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

    static void throwFromRegisterMr()
    {
        auto &funcs        = hipObj::ibv.funcsForTest();
        funcs.reg_mr       = &throwingRegisterMr;
        funcs.reg_mr_iova2 = &throwingRegisterMrIova2;
    }
};

TEST_P(ApiExceptionTest, Init)
{
    hipObj::hipOps().hipGetDevice = &throwingGetDevice;
    hipObjConfig_t config         = makeConfig();
    EXPECT_EQ(hipObjInit(&config).opError, hipObjInternalError);
    EXPECT_FALSE(state_.initialized);
}

TEST_P(ApiExceptionTest, BufRegister)
{
    ASSERT_NO_FATAL_FAILURE(init());
    throwFromRegisterMr();
    EXPECT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjInternalError);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjBufNotRegistered);
}

TEST_P(ApiExceptionTest, BufRegisterHost)
{
    ASSERT_NO_FATAL_FAILURE(init());
    throwFromRegisterMr();
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
    ASSERT_NO_FATAL_FAILURE(init());
    /* Registered through a staging buffer, so a sync copies */
    fake().fail.registerAddr = kDevBuf;
    ASSERT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjSuccess);
    hipObj::hipOps().hipMemcpy = &throwingMemcpy;
    EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize, 0, HIPOBJ_SYNC_TO_DEVICE).opError, hipObjInternalError);
}

INSTANTIATE_TEST_SUITE_P(ApiException, ApiExceptionTest, ::testing::Values(Thrown::StdException, Thrown::Int),
                         [](const ::testing::TestParamInfo<Thrown> &paramInfo) {
                             return paramInfo.param == Thrown::StdException ? "StdException" : "Int";
                         });

} // namespace
