/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Argument checks in the public API. Every function in hipobj.h is called
 * with each kind of bad argument, and with arguments just inside each
 * limit, without GPU or RDMA hardware, using the fake device in
 * fake-device.h.
 */

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>
#include <sys/types.h>

#include "fake-device.h"
#include "hip-seam.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "state.h"
#include "token.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::fakeMemGetAddressRange;
using hipObjTest::fakePointerGetAttributes;
using hipObjTest::makeOps;

/* Fake device addresses. reg_mr is faked, so nothing reads or writes
 * them. */
void *const kDevBuf = hipObjTest::fakeDevBuf();

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;
constexpr size_t kMaxMr   = 4ULL * 1024 * 1024 * 1024;

// ---- Fixture -----------------------------------------------------

class ApiArgsTest : public hipObjTest::FakeDeviceTest {};

// ---- hipObjGetErrorString ----------------------------------------

TEST(ApiArgsErrorString, EveryCodeHasItsOwnString)
{
    std::vector<hipObjOpError_t> codes = {
        hipObjSuccess,          hipObjInvalidValue,
        hipObjNotInitialized,   hipObjAlreadyInitialized,
        hipObjRdmaError,        hipObjS3Error,
        hipObjBufNotRegistered, hipObjBufAlreadyRegistered,
        hipObjNicNotFound,      hipObjDmabufNotSupported,
        hipObjSizeTooLarge,     hipObjInternalError,
#ifdef HIPOBJECT_V2_API
        hipObjNotSupported,     hipObjBusy,
#endif
    };
    std::vector<std::string> seen;
    for (hipObjOpError_t code : codes) {
        const char *str = hipObjGetErrorString(code);
        ASSERT_NE(str, nullptr);
        EXPECT_STRNE(str, "Unknown error") << "code " << code;
        for (const std::string &other : seen) {
            EXPECT_NE(other, str) << "code " << code;
        }
        seen.emplace_back(str);
    }
}

TEST(ApiArgsErrorString, UnknownCode)
{
    /* A C caller can pass any int. Copy one in the way C would see it,
     * rather than casting a value no enumerator has. 15 stays within the
     * 4 bits the enumerators need, which keeps it a valid value in C++. */
    static_assert(sizeof(hipObjOpError_t) == sizeof(int));
    const int       raw = 15;
    hipObjOpError_t code;
    std::memcpy(&code, &raw, sizeof(code));
    EXPECT_STREQ(hipObjGetErrorString(code), "Unknown error");
}

// ---- Calls before hipObjInit() -----------------------------------

TEST_F(ApiArgsTest, EverythingButInitNeedsInit)
{
    char        buf[kBufSize] = {};
    hipObjOps_t ops           = makeOps();
    char       *token         = nullptr;

    EXPECT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjNotInitialized);
    EXPECT_EQ(hipObjBufRegisterHost(buf, sizeof(buf)).opError, hipObjNotInitialized);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjNotInitialized);
    EXPECT_EQ(hipObjGet(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjNotInitialized);
    EXPECT_EQ(hipObjPut(nullptr, kDevBuf, kBufSize, 0, &ops, nullptr).opError, hipObjNotInitialized);
    EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjNotInitialized);
    EXPECT_EQ(hipObjGetRdmaToken(kDevBuf, kBufSize, HIPOBJ_RDMA_OP_PUT, &token).opError,
              hipObjNotInitialized);
    EXPECT_EQ(token, nullptr);
    EXPECT_EQ(fake().log.sendRequest, 0);

    /* Shutting down an uninitialized library is allowed */
    EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
}

// ---- hipObjInit --------------------------------------------------

TEST_F(ApiArgsTest, InitRejectsNullConfig)
{
    EXPECT_EQ(hipObjInit(nullptr).opError, hipObjInvalidValue);
    EXPECT_FALSE(state_.initialized);
}

TEST_F(ApiArgsTest, InitRejectsReservedFlags)
{
    for (uint32_t flags : {1U, 0x80000000U, 0xffffffffU}) {
        hipObjConfig_t config = makeConfig();
        config.flags          = flags;
        EXPECT_EQ(hipObjInit(&config).opError, hipObjInvalidValue) << "flags " << flags;
        EXPECT_FALSE(state_.initialized);
    }
}

TEST_F(ApiArgsTest, InitRejectsNegativeDeviceOtherThanMinusOne)
{
    for (int device : {-2, std::numeric_limits<int>::min()}) {
        hipObjConfig_t config = makeConfig();
        config.gpuDevice      = device;
        EXPECT_EQ(hipObjInit(&config).opError, hipObjInvalidValue) << "device " << device;
        EXPECT_FALSE(state_.initialized);
    }
}

TEST_F(ApiArgsTest, InitRejectsDeviceThatDoesNotExist)
{
    fake().deviceCount = 2;
    for (int device : {2, 3, std::numeric_limits<int>::max()}) {
        hipObjConfig_t config = makeConfig();
        config.gpuDevice      = device;
        EXPECT_EQ(hipObjInit(&config).opError, hipObjInvalidValue) << "device " << device;
        EXPECT_FALSE(state_.initialized);
    }

    hipObjConfig_t config = makeConfig();
    config.gpuDevice      = 1;
    EXPECT_EQ(hipObjInit(&config).opError, hipObjSuccess);
    EXPECT_EQ(state_.gpuDevice, 1);
}

TEST_F(ApiArgsTest, InitRejectsExplicitDeviceWithoutGpus)
{
    fake().deviceCountErr = hipErrorNoDevice;
    hipObjConfig_t config = makeConfig();
    config.gpuDevice      = 0;
    EXPECT_EQ(hipObjInit(&config).opError, hipObjInvalidValue);
    EXPECT_FALSE(state_.initialized);
}

TEST_F(ApiArgsTest, InitReportsDeviceCountFailure)
{
    fake().deviceCountErr = hipErrorInvalidValue;
    hipObjConfig_t config = makeConfig();
    config.gpuDevice      = 0;

    hipObjError_t err = hipObjInit(&config);

    EXPECT_EQ(err.opError, hipObjRdmaError);
    EXPECT_EQ(err.hipError, static_cast<int>(hipErrorInvalidValue));
    EXPECT_FALSE(state_.initialized);
}

TEST_F(ApiArgsTest, InitAcceptsOptionalFieldsUnset)
{
    /* endpoint, region, the keys, and the NIC hint may all be NULL */
    hipObjConfig_t config = {};
    config.gpuDevice      = 0;
    EXPECT_EQ(hipObjInit(&config).opError, hipObjNicNotFound);

    config.nicHint = "mlx5_0";
    EXPECT_EQ(hipObjInit(&config).opError, hipObjSuccess);
}

TEST_F(ApiArgsTest, InitTwiceFails)
{
    ASSERT_NO_FATAL_FAILURE(init());
    hipObjConfig_t config = makeConfig();
    EXPECT_EQ(hipObjInit(&config).opError, hipObjAlreadyInitialized);
}

// ---- hipObjBufRegister and hipObjBufRegisterHost -----------------

using RegisterFn = hipObjError_t (*)(void *, size_t);

class RegisterArgsTest : public ApiArgsTest, public ::testing::WithParamInterface<RegisterFn> {};

TEST_P(RegisterArgsTest, RejectsNullPointer)
{
    ASSERT_NO_FATAL_FAILURE(init());
    EXPECT_EQ(GetParam()(nullptr, kBufSize).opError, hipObjInvalidValue);
    EXPECT_EQ(fake().log.registerMr, 0);
}

TEST_P(RegisterArgsTest, RejectsZeroSize)
{
    ASSERT_NO_FATAL_FAILURE(init());
    EXPECT_EQ(GetParam()(kDevBuf, 0).opError, hipObjInvalidValue);
    EXPECT_EQ(fake().log.registerMr, 0);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjBufNotRegistered);
}

TEST_P(RegisterArgsTest, RejectsMoreThanMaxSize)
{
    ASSERT_NO_FATAL_FAILURE(init());
    for (size_t size : {kMaxMr + 1, std::numeric_limits<size_t>::max()}) {
        EXPECT_EQ(GetParam()(kDevBuf, size).opError, hipObjSizeTooLarge) << "size " << size;
    }
    EXPECT_EQ(fake().log.registerMr, 0);

    EXPECT_EQ(GetParam()(kDevBuf, kMaxMr).opError, hipObjSuccess);
}

TEST_P(RegisterArgsTest, RejectsBufferThatWrapsAddressSpace)
{
    ASSERT_NO_FATAL_FAILURE(init());
    void *top = reinterpret_cast<void *>(std::numeric_limits<uintptr_t>::max() - 15);
    EXPECT_EQ(GetParam()(top, 16).opError, hipObjInvalidValue);
    EXPECT_EQ(GetParam()(top, kMaxMr).opError, hipObjInvalidValue);
    EXPECT_EQ(fake().log.registerMr, 0);

    EXPECT_EQ(GetParam()(top, 15).opError, hipObjSuccess);
}

TEST_P(RegisterArgsTest, RejectsSecondRegistration)
{
    ASSERT_NO_FATAL_FAILURE(init());
    ASSERT_EQ(GetParam()(kDevBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjBufAlreadyRegistered);
    EXPECT_EQ(hipObjBufRegisterHost(kDevBuf, kBufSize).opError, hipObjBufAlreadyRegistered);
    EXPECT_EQ(fake().log.registerMr, 1);
}

INSTANTIATE_TEST_SUITE_P(ApiArgs, RegisterArgsTest,
                         ::testing::Values(&hipObjBufRegister, &hipObjBufRegisterHost),
                         [](const ::testing::TestParamInfo<RegisterFn> &paramInfo) {
                             return paramInfo.param == &hipObjBufRegister ? "Device" : "Host";
                         });

// ---- hipObjBufRegister: range of the HIP allocation --------------

/* The HIP runtime reports every pointer as device memory in one
 * allocation, [kAllocBase, kAllocBase + kAllocSize) */
class DeviceRangeTest : public ApiArgsTest {
protected:
    static constexpr uintptr_t kAllocBase = 0x200000;
    static constexpr size_t    kAllocSize = 4096;

    void SetUp() override
    {
        ApiArgsTest::SetUp();
        hipObj::HipOps &ops         = hipObj::hipOps();
        ops.hipPointerGetAttributes = &fakePointerGetAttributes;
        ops.hipMemGetAddressRange   = &fakeMemGetAddressRange;

        fake().memoryType        = hipMemoryTypeDevice;
        fake().allocBase         = kAllocBase;
        fake().allocSize         = kAllocSize;
        fake().addressRangeErr   = hipSuccess;
        fake().addressRangeCalls = 0;
    }

    static void *at(size_t offset)
    {
        return reinterpret_cast<void *>(kAllocBase + offset);
    }
};

TEST_F(DeviceRangeTest, AcceptsRangeInsideAllocation)
{
    ASSERT_NO_FATAL_FAILURE(init());
    EXPECT_EQ(hipObjBufRegister(at(0), kAllocSize).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufDeregister(at(0)).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufRegister(at(16), kAllocSize - 16).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufDeregister(at(16)).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufRegister(at(kAllocSize - 1), 1).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.registerMr, 3);
}

TEST_F(DeviceRangeTest, RejectsRangePastEndOfAllocation)
{
    ASSERT_NO_FATAL_FAILURE(init());
    EXPECT_EQ(hipObjBufRegister(at(0), kAllocSize + 1).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjBufRegister(at(16), kAllocSize - 15).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjBufRegister(at(kAllocSize - 1), 2).opError, hipObjInvalidValue);
    EXPECT_EQ(fake().log.registerMr, 0);
    EXPECT_EQ(hipObjBufDeregister(at(0)).opError, hipObjBufNotRegistered);
}

TEST_F(DeviceRangeTest, RejectsPointerOutsideReportedAllocation)
{
    ASSERT_NO_FATAL_FAILURE(init());
    /* A runtime that reports an allocation that doesn't hold the pointer */
    EXPECT_EQ(hipObjBufRegister(reinterpret_cast<void *>(kAllocBase - 1), 1).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjBufRegister(at(kAllocSize), 1).opError, hipObjInvalidValue);
    EXPECT_EQ(fake().log.registerMr, 0);
}

TEST_F(DeviceRangeTest, RejectsBufferWhoseAllocationIsNotFound)
{
    ASSERT_NO_FATAL_FAILURE(init());
    fake().addressRangeErr = hipErrorNotFound;
    EXPECT_EQ(hipObjBufRegister(at(0), kAllocSize).opError, hipObjInvalidValue);

    hipObj::hipOps().hipMemGetAddressRange = nullptr;
    EXPECT_EQ(hipObjBufRegister(at(0), kAllocSize).opError, hipObjInvalidValue);
    EXPECT_EQ(fake().log.registerMr, 0);
}

TEST_F(DeviceRangeTest, DoesNotCheckOtherMemory)
{
    ASSERT_NO_FATAL_FAILURE(init());
    /* The NIC's registration checks that host memory is mapped */
    fake().memoryType = hipMemoryTypeHost;
    EXPECT_EQ(hipObjBufRegister(at(0), kAllocSize + 1).opError, hipObjSuccess);
    EXPECT_EQ(fake().addressRangeCalls, 0);

    /* hipObjBufRegisterHost() takes host memory, so it doesn't look */
    fake().memoryType = hipMemoryTypeDevice;
    EXPECT_EQ(hipObjBufRegisterHost(at(16), kAllocSize).opError, hipObjSuccess);
    EXPECT_EQ(fake().addressRangeCalls, 0);
}

// ---- hipObjBufDeregister -----------------------------------------

TEST_F(ApiArgsTest, DeregisterRejectsNullPointer)
{
    ASSERT_NO_FATAL_FAILURE(init());
    EXPECT_EQ(hipObjBufDeregister(nullptr).opError, hipObjInvalidValue);
}

TEST_F(ApiArgsTest, DeregisterRejectsUnregisteredBuffer)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    /* A pointer into a registration isn't the registration */
    EXPECT_EQ(hipObjBufDeregister(static_cast<char *>(kDevBuf) + 1).opError, hipObjBufNotRegistered);

    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjBufNotRegistered);
}

// ---- hipObjGet and hipObjPut -------------------------------------

using TransferFn = hipObjError_t (*)(void *, size_t, off_t, hipObjOps_t *);

hipObjError_t
callGet(void *devPtr, size_t size, off_t offset, hipObjOps_t *ops)
{
    return hipObjGet(nullptr, devPtr, size, offset, ops, nullptr);
}

hipObjError_t
callPut(void *devPtr, size_t size, off_t offset, hipObjOps_t *ops)
{
    return hipObjPut(nullptr, devPtr, size, offset, ops, nullptr);
}

class TransferArgsTest : public ApiArgsTest, public ::testing::WithParamInterface<TransferFn> {
protected:
    void expectRejected(void *devPtr, size_t size, off_t offset, hipObjOps_t *ops, hipObjOpError_t expected)
    {
        EXPECT_EQ(GetParam()(devPtr, size, offset, ops).opError, expected)
            << "size " << size << ", offset " << offset;
        EXPECT_EQ(fake().log.sendRequest, 0);
        EXPECT_EQ(fake().log.recvReply, 0);
        EXPECT_EQ(fake().log.memcpy, 0);
    }
};

TEST_P(TransferArgsTest, RejectsMissingCallbacks)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    expectRejected(kDevBuf, kBufSize, 0, nullptr, hipObjInvalidValue);

    hipObjOps_t ops = makeOps();
    ops.sendRequest = nullptr;
    expectRejected(kDevBuf, kBufSize, 0, &ops, hipObjInvalidValue);

    ops           = makeOps();
    ops.recvReply = nullptr;
    expectRejected(kDevBuf, kBufSize, 0, &ops, hipObjInvalidValue);
}

TEST_P(TransferArgsTest, RejectsNullPointer)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    expectRejected(nullptr, kBufSize, 0, &ops, hipObjInvalidValue);
}

TEST_P(TransferArgsTest, RejectsZeroSize)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    expectRejected(kDevBuf, 0, 0, &ops, hipObjInvalidValue);
}

TEST_P(TransferArgsTest, RejectsNegativeOffset)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    expectRejected(kDevBuf, 1, -1, &ops, hipObjInvalidValue);
    expectRejected(kDevBuf, 1, std::numeric_limits<off_t>::min(), &ops, hipObjInvalidValue);
}

TEST_P(TransferArgsTest, RejectsUnregisteredBuffer)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    expectRejected(reinterpret_cast<void *>(0x20000), kBufSize, 0, &ops, hipObjBufNotRegistered);
    /* A pointer into a registration isn't the registration */
    expectRejected(static_cast<char *>(kDevBuf) + 1, 1, 0, &ops, hipObjBufNotRegistered);
}

TEST_P(TransferArgsTest, RejectsRangeOutsideBuffer)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    expectRejected(kDevBuf, kBufSize + 1, 0, &ops, hipObjInvalidValue);
    expectRejected(kDevBuf, 2, kBufSize - 1, &ops, hipObjInvalidValue);
    expectRejected(kDevBuf, 1, kBufSize, &ops, hipObjInvalidValue);
    expectRejected(kDevBuf, 1, std::numeric_limits<off_t>::max(), &ops, hipObjInvalidValue);
}

TEST_P(TransferArgsTest, RejectsRangeThatWraps)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    /* offset + size wraps to 0 */
    expectRejected(kDevBuf, std::numeric_limits<size_t>::max(), 1, &ops, hipObjInvalidValue);
    expectRejected(kDevBuf, std::numeric_limits<size_t>::max() - kBufSize + 2, kBufSize - 1, &ops,
                   hipObjInvalidValue);
}

TEST_P(TransferArgsTest, AcceptsRangeAtEndOfBuffer)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    hipObjOps_t ops = makeOps();
    EXPECT_EQ(GetParam()(kDevBuf, 1, kBufSize - 1, &ops).opError, hipObjSuccess);
    EXPECT_EQ(GetParam()(kDevBuf, kBufSize, 0, &ops).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.sendRequest, 2);
    EXPECT_EQ(fake().log.recvReply, 2);
}

TEST_P(TransferArgsTest, ChecksStagedBuffersBeforeCopying)
{
    ASSERT_NO_FATAL_FAILURE(init());
    void *staged             = reinterpret_cast<void *>(0x30000);
    fake().fail.registerAddr = staged;
    ASSERT_EQ(hipObjBufRegister(staged, kBufSize).opError, hipObjSuccess);

    hipObjOps_t ops = makeOps();
    expectRejected(staged, 2, kBufSize - 1, &ops, hipObjInvalidValue);
    expectRejected(staged, std::numeric_limits<size_t>::max(), 1, &ops, hipObjInvalidValue);

    EXPECT_EQ(GetParam()(staged, 1, kBufSize - 1, &ops).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.memcpy, 1);
}

INSTANTIATE_TEST_SUITE_P(ApiArgs, TransferArgsTest, ::testing::Values(&callGet, &callPut),
                         [](const ::testing::TestParamInfo<TransferFn> &paramInfo) {
                             return paramInfo.param == &callGet ? "Get" : "Put";
                         });

// ---- hipObjBufSync -----------------------------------------------

TEST_F(ApiArgsTest, SyncRejectsBadDirection)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    for (int direction : {-1, 2, std::numeric_limits<int>::max()}) {
        EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize, 0, direction).opError, hipObjInvalidValue)
            << "direction " << direction;
    }
}

TEST_F(ApiArgsTest, SyncRejectsBadBufferArguments)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    for (int direction : {HIPOBJ_SYNC_TO_HOST, HIPOBJ_SYNC_TO_DEVICE}) {
        EXPECT_EQ(hipObjBufSync(nullptr, kBufSize, 0, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(kDevBuf, 0, 0, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(kDevBuf, 1, -1, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(static_cast<char *>(kDevBuf) + 1, 1, 0, direction).opError,
                  hipObjBufNotRegistered);
    }
}

TEST_F(ApiArgsTest, SyncRejectsRangeOutsideDirectBuffer)
{
    /* A directly registered buffer needs no staging, but a bad range is
     * still an error */
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    for (int direction : {HIPOBJ_SYNC_TO_HOST, HIPOBJ_SYNC_TO_DEVICE}) {
        EXPECT_EQ(hipObjBufSync(kDevBuf, kBufSize + 1, 0, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(kDevBuf, 1, kBufSize, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(kDevBuf, std::numeric_limits<size_t>::max(), 1, direction).opError,
                  hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(kDevBuf, 1, kBufSize - 1, direction).opError, hipObjSuccess);
    }
    EXPECT_EQ(fake().log.memcpy, 0);
}

TEST_F(ApiArgsTest, SyncRejectsRangeOutsideStagedBuffer)
{
    ASSERT_NO_FATAL_FAILURE(init());
    void *staged             = reinterpret_cast<void *>(0x30000);
    fake().fail.registerAddr = staged;
    ASSERT_EQ(hipObjBufRegister(staged, kBufSize).opError, hipObjSuccess);

    for (int direction : {HIPOBJ_SYNC_TO_HOST, HIPOBJ_SYNC_TO_DEVICE}) {
        EXPECT_EQ(hipObjBufSync(staged, kBufSize + 1, 0, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(staged, std::numeric_limits<size_t>::max(), 1, direction).opError,
                  hipObjInvalidValue);
    }
    EXPECT_EQ(fake().log.memcpy, 0);

    EXPECT_EQ(hipObjBufSync(staged, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufSync(staged, 1, kBufSize - 1, HIPOBJ_SYNC_TO_DEVICE).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.memcpy, 2);
}

// ---- hipObjGetRdmaToken and hipObjPutRdmaToken -------------------

TEST_F(ApiArgsTest, GetTokenRejectsBadArguments)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    char *token = nullptr;
    EXPECT_EQ(hipObjGetRdmaToken(kDevBuf, kBufSize, HIPOBJ_RDMA_OP_PUT, nullptr).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjGetRdmaToken(nullptr, kBufSize, HIPOBJ_RDMA_OP_PUT, &token).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjGetRdmaToken(kDevBuf, 0, HIPOBJ_RDMA_OP_PUT, &token).opError, hipObjInvalidValue);
    for (int op : {-1, 2, std::numeric_limits<int>::max()}) {
        EXPECT_EQ(hipObjGetRdmaToken(kDevBuf, kBufSize, op, &token).opError, hipObjInvalidValue)
            << "op " << op;
    }
    EXPECT_EQ(hipObjGetRdmaToken(static_cast<char *>(kDevBuf) + 1, 1, HIPOBJ_RDMA_OP_PUT, &token).opError,
              hipObjBufNotRegistered);
    EXPECT_EQ(hipObjGetRdmaToken(kDevBuf, kBufSize + 1, HIPOBJ_RDMA_OP_PUT, &token).opError,
              hipObjInvalidValue);
    EXPECT_EQ(token, nullptr);
}

TEST_F(ApiArgsTest, GetTokenAcceptsWholeBuffer)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegister());
    for (int op : {HIPOBJ_RDMA_OP_PUT, HIPOBJ_RDMA_OP_GET}) {
        char *token = nullptr;
        ASSERT_EQ(hipObjGetRdmaToken(kDevBuf, kBufSize, op, &token).opError, hipObjSuccess);
        ASSERT_NE(token, nullptr);
        EXPECT_EQ(std::strlen(token), 88U);
        EXPECT_EQ(hipObjPutRdmaToken(token).opError, hipObjSuccess);
    }
}

TEST(ApiArgsToken, PutTokenRejectsNull)
{
    EXPECT_EQ(hipObjPutRdmaToken(nullptr).opError, hipObjInvalidValue);
}

// ---- hipObjParseRdmaReply ----------------------------------------

std::string
validTokenHex(uint8_t transport = hipObj::TRANSPORT_RC)
{
    hipObj::RdmaToken token;
    token.transport = transport;
    token.qpNum     = 7;
    return hipObj::encodeRdmaToken(token);
}

hipObjOpError_t
parseReply(const std::string &reply, int &httpCode)
{
    return hipObjParseRdmaReply(reply.data(), reply.size(), &httpCode).opError;
}

TEST(ApiArgsParseReply, RejectsNullArguments)
{
    int httpCode = 0;
    EXPECT_EQ(hipObjParseRdmaReply(nullptr, 3, &httpCode).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjParseRdmaReply("200", 3, nullptr).opError, hipObjInvalidValue);
}

TEST(ApiArgsParseReply, AcceptsDocumentedForms)
{
    const struct {
        std::string reply;
        int         httpCode;
    } cases[] = {
        {"ok", 200},
        {"err", -1},
        {"200", 200},
        {"204", 204},
        {"206", 206},
        {"404", 404},
        {"501", 501},
        {"100", 100},
        {"599", 599},
        {"200:" + validTokenHex(), 200},
        {"200:" + validTokenHex(hipObj::TRANSPORT_DC), 200},
        /* Hex is case-insensitive */
        {"200:" + std::string(88, 'A').replace(0, 2, "01"), 200},
        /* Trailing line endings and NULs are ignored */
        {"200\r\n", 200},
        {std::string("200\0\0", 5), 200},
        {"ok\n", 200},
    };
    for (const auto &c : cases) {
        int httpCode = 0;
        EXPECT_EQ(parseReply(c.reply, httpCode), hipObjSuccess) << "reply \"" << c.reply << "\"";
        EXPECT_EQ(httpCode, c.httpCode) << "reply \"" << c.reply << "\"";
    }
}

TEST(ApiArgsParseReply, ReadsOnlyReplyLen)
{
    int httpCode = 0;
    EXPECT_EQ(hipObjParseRdmaReply("2004", 3, &httpCode).opError, hipObjSuccess);
    EXPECT_EQ(httpCode, 200);
}

TEST(ApiArgsParseReply, RejectsMalformedReplies)
{
    const std::string token   = validTokenHex();
    const std::string cases[] = {
        "",
        "\r\n",
        "okay",
        "ok ",
        "OK",
        "error",
        "20",
        "2000",
        "0200",
        "+200",
        "-200",
        " 200",
        "200 ",
        "099",
        "600",
        "999",
        "abc",
        "2x0",
        /* Embedded NUL */
        std::string{'2', '\0', '0', '0'},
        "200\r\n200",
        "200:",
        ":" + token,
        "200:" + token.substr(1),
        "200:" + token + "0",
        "200:" + token + ":1:2",
        "200: " + token.substr(1),
        "200:" + std::string(88, 'g'),
        /* Unknown transport byte */
        "200:" + validTokenHex(0x02),
        "200:" + validTokenHex(0xff),
        /* Only a 200 reply carries a token */
        "206:" + token,
        "404:" + token,
        "501:" + token,
        "ok:" + token,
    };
    for (const std::string &reply : cases) {
        int httpCode = 12345;
        EXPECT_EQ(parseReply(reply, httpCode), hipObjInvalidValue) << "reply \"" << reply << "\"";
        EXPECT_EQ(httpCode, 12345) << "reply \"" << reply << "\"";
    }
}

// ---- hipObjTokenClientNic ----------------------------------------

std::string
tokenWithIpv4()
{
    hipObj::RdmaToken token;
    token.transport = hipObj::TRANSPORT_RC;
    token.gid[10]   = 0xff;
    token.gid[11]   = 0xff;
    token.gid[12]   = 192;
    token.gid[13]   = 168;
    token.gid[14]   = 100;
    token.gid[15]   = 200;
    return hipObj::encodeRdmaToken(token);
}

TEST(ApiArgsTokenClientNic, RejectsNullAndEmptyArguments)
{
    const std::string token     = tokenWithIpv4();
    char              nicIp[32] = "unchanged";
    EXPECT_EQ(hipObjTokenClientNic(nullptr, nicIp, sizeof(nicIp)).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjTokenClientNic(token.c_str(), nullptr, sizeof(nicIp)).opError, hipObjInvalidValue);
    EXPECT_EQ(hipObjTokenClientNic(token.c_str(), nicIp, 0).opError, hipObjInvalidValue);
}

TEST(ApiArgsTokenClientNic, RejectsMalformedTokens)
{
    const std::string token   = tokenWithIpv4();
    const std::string cases[] = {
        "",
        token.substr(1),
        token + "0",
        token + ":1:2",
        std::string(88, 'g'),
        "zz" + token.substr(2),
        /* Unknown transport byte */
        "02" + token.substr(2),
    };
    for (const std::string &bad : cases) {
        char nicIp[32] = "unchanged";
        EXPECT_EQ(hipObjTokenClientNic(bad.c_str(), nicIp, sizeof(nicIp)).opError, hipObjInvalidValue)
            << "token \"" << bad << "\"";
        EXPECT_STREQ(nicIp, "");
    }
}

TEST(ApiArgsTokenClientNic, RespectsBufferLength)
{
    const std::string token = tokenWithIpv4();
    /* "192.168.100.200" is 15 characters */
    char nicIp[16] = "unchanged";
    EXPECT_EQ(hipObjTokenClientNic(token.c_str(), nicIp, 15).opError, hipObjInvalidValue);
    EXPECT_STREQ(nicIp, "");
    EXPECT_EQ(hipObjTokenClientNic(token.c_str(), nicIp, 1).opError, hipObjInvalidValue);
    EXPECT_STREQ(nicIp, "");

    EXPECT_EQ(hipObjTokenClientNic(token.c_str(), nicIp, sizeof(nicIp)).opError, hipObjSuccess);
    EXPECT_STREQ(nicIp, "192.168.100.200");
}

TEST(ApiArgsTokenClientNic, EmptyWhenNoIpv4Address)
{
    const std::string token     = validTokenHex();
    char              nicIp[16] = "unchanged";
    EXPECT_EQ(hipObjTokenClientNic(token.c_str(), nicIp, sizeof(nicIp)).opError, hipObjSuccess);
    EXPECT_STREQ(nicIp, "");
}

#ifdef HIPOBJECT_V2_API

// ---- hipObjGetV2 and hipObjPutV2 ---------------------------------

int
fakeSendPrepare(void *, const hipObjTransferReqV2_t *, hipObjPrepareReplyV2_t *)
{
    return 0;
}

int
fakeSendReady(void *, const hipObjTransferReqV2_t *, hipObjFinalReplyV2_t *)
{
    return 0;
}

int
fakeSendCancel(void *, const hipObjTransferReqV2_t *)
{
    return 0;
}

hipObjOpsV2_t
makeOpsV2()
{
    hipObjOpsV2_t ops = {};
    ops.sendPrepare   = &fakeSendPrepare;
    ops.sendReady     = &fakeSendReady;
    ops.sendCancel    = &fakeSendCancel;
    return ops;
}

using TransferV2Fn = hipObjError_t (*)(const char *, const char *, void *, uint64_t, uint64_t, const char *,
                                       hipObjOpsV2_t *);

hipObjError_t
callGetV2(const char *bucket, const char *key, void *devPtr, uint64_t size, uint64_t offset,
          const char *query, hipObjOpsV2_t *ops)
{
    return hipObjGetV2(bucket, key, devPtr, size, offset, query, ops, nullptr);
}

hipObjError_t
callPutV2(const char *bucket, const char *key, void *devPtr, uint64_t size, uint64_t offset,
          const char *query, hipObjOpsV2_t *ops)
{
    return hipObjPutV2(bucket, key, devPtr, size, offset, query, ops, nullptr);
}

constexpr uint64_t kMaxV2Size = 0x7fffffff;

/* The V2 entry points aren't implemented, so valid arguments get
 * hipObjNotSupported */
class TransferV2ArgsTest : public ::testing::TestWithParam<TransferV2Fn> {
protected:
    hipObjOpError_t call(const char *bucket, const char *key, uint64_t size = kBufSize, uint64_t offset = 0,
                         const char *query = nullptr)
    {
        return GetParam()(bucket, key, kDevBuf, size, offset, query, &ops_).opError;
    }

    hipObjOpsV2_t ops_ = makeOpsV2();
};

TEST_P(TransferV2ArgsTest, AcceptsValidArguments)
{
    EXPECT_EQ(call("bucket", "key"), hipObjNotSupported);
    EXPECT_EQ(call("b", "a/b/c.bin", 1, 0, ""), hipObjNotSupported);
}

TEST_P(TransferV2ArgsTest, RejectsBadBucket)
{
    EXPECT_EQ(call(nullptr, "key"), hipObjInvalidValue);
    EXPECT_EQ(call("", "key"), hipObjInvalidValue);
    EXPECT_EQ(call(std::string(256, 'b').c_str(), "key"), hipObjInvalidValue);
    EXPECT_EQ(call(std::string(255, 'b').c_str(), "key"), hipObjNotSupported);
}

TEST_P(TransferV2ArgsTest, RejectsBadKey)
{
    EXPECT_EQ(call("bucket", nullptr), hipObjInvalidValue);
    EXPECT_EQ(call("bucket", ""), hipObjInvalidValue);
    EXPECT_EQ(call("bucket", std::string(1025, 'k').c_str()), hipObjInvalidValue);
    EXPECT_EQ(call("bucket", std::string(1024, 'k').c_str()), hipObjNotSupported);
}

TEST_P(TransferV2ArgsTest, RejectsNullPointer)
{
    EXPECT_EQ(GetParam()("bucket", "key", nullptr, kBufSize, 0, nullptr, &ops_).opError, hipObjInvalidValue);
}

TEST_P(TransferV2ArgsTest, ChecksSize)
{
    EXPECT_EQ(call("bucket", "key", 0), hipObjInvalidValue);
    EXPECT_EQ(call("bucket", "key", kMaxV2Size + 1), hipObjSizeTooLarge);
    EXPECT_EQ(call("bucket", "key", std::numeric_limits<uint64_t>::max()), hipObjSizeTooLarge);
    EXPECT_EQ(call("bucket", "key", kMaxV2Size), hipObjNotSupported);
}

TEST_P(TransferV2ArgsTest, RejectsRangeThatWraps)
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(call("bucket", "key", 1, max), hipObjInvalidValue);
    EXPECT_EQ(call("bucket", "key", kBufSize, max - kBufSize + 1), hipObjInvalidValue);
    EXPECT_EQ(call("bucket", "key", kBufSize, max - kBufSize), hipObjNotSupported);
}

TEST_P(TransferV2ArgsTest, RejectsMissingCallbacks)
{
    EXPECT_EQ(GetParam()("bucket", "key", kDevBuf, kBufSize, 0, nullptr, nullptr).opError,
              hipObjInvalidValue);

    ops_.sendPrepare = nullptr;
    EXPECT_EQ(call("bucket", "key"), hipObjInvalidValue);

    ops_           = makeOpsV2();
    ops_.sendReady = nullptr;
    EXPECT_EQ(call("bucket", "key"), hipObjInvalidValue);

    ops_            = makeOpsV2();
    ops_.sendCancel = nullptr;
    EXPECT_EQ(call("bucket", "key"), hipObjInvalidValue);
}

TEST_P(TransferV2ArgsTest, AcceptsCanonicalQuery)
{
    for (const char *query :
         {"partNumber=10000&uploadId=abc-DEF_1.2~3", "uploadId=a%2Bb%2Fc%3D%3D", "a=%20%7F%80%FF",
          "a=", "a=&b=", "a=1&a=2", "a=1&a=1", "a=&a=1", "a=9&a-b=1", "A=1&a=1", "a%2F=1&a-=1"}) {
        EXPECT_EQ(call("bucket", "key", kBufSize, 0, query), hipObjNotSupported)
            << "query \"" << query << "\"";
    }
}

TEST_P(TransferV2ArgsTest, RejectsQueryThatIsNotCanonical)
{
    /* Characters that must be encoded, and bad escapes */
    for (const char *query :
         {"a=b c", "a=b\r\nX-Injected: 1", "a=b\n", "a=b\t", "a=b/c", "a=b+c", "a=b#c", "?a=b", "a=b=c",
          "a=%", "a=%2", "a=%G0", "a=%0G", "a=%2f", "a=\x80", "a=\x7f"}) {
        EXPECT_EQ(call("bucket", "key", kBufSize, 0, query), hipObjInvalidValue)
            << "query \"" << query << "\"";
    }
    /* Escapes of unreserved characters, which are written as is */
    for (const char *query : {"a=%41", "a=%7A", "a=%30", "a=%2D", "a=%2E", "a=%5F", "a=%7E", "%61=1"}) {
        EXPECT_EQ(call("bucket", "key", kBufSize, 0, query), hipObjInvalidValue)
            << "query \"" << query << "\"";
    }
    /* Empty parameters and keys, and parameters without '=' */
    for (const char *query :
         {"&", "a=1&", "&a=1", "a=1&&b=2", "=1", "a=1&=2", "a", "a&b=", "a=1&b", "a&a=1"}) {
        EXPECT_EQ(call("bucket", "key", kBufSize, 0, query), hipObjInvalidValue)
            << "query \"" << query << "\"";
    }
    /* Parameters out of order, by key and then by value */
    for (const char *query :
         {"uploadId=x&partNumber=1", "b=&a=", "a=2&a=1", "a=1&a=", "a-b=1&a=9", "a=1&A=1"}) {
        EXPECT_EQ(call("bucket", "key", kBufSize, 0, query), hipObjInvalidValue)
            << "query \"" << query << "\"";
    }
}

INSTANTIATE_TEST_SUITE_P(ApiArgs, TransferV2ArgsTest, ::testing::Values(&callGetV2, &callPutV2),
                         [](const ::testing::TestParamInfo<TransferV2Fn> &paramInfo) {
                             return paramInfo.param == &callGetV2 ? "GetV2" : "PutV2";
                         });

#endif /* HIPOBJECT_V2_API */

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
