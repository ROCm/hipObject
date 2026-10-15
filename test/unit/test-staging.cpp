/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Buffers the NIC can't reach directly: registering them through a host
 * staging buffer, and copying between the staging buffer and the caller's
 * memory with a deadline, against the fake device in fake-device.h. The
 * staging timeout and strict GPU-direct mode come from the options, which
 * test-state.cpp tests.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <limits>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "fake-device.h"
#include "hipobj-address.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "state.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::makeOps;

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

/* reg_mr fails for this address, so it's registered through a staging
 * buffer */
void *const kStagedBuf = hipObj::addressToPointer<void *>(0x30000);

/* The HIP runtime reports every pointer as device memory in one allocation,
 * which holds every buffer these tests register */
void
fakeDeviceMemory()
{
    fake().memoryType = hipMemoryTypeDevice;
    fake().allocBase  = 0;
    fake().allocSize  = std::numeric_limits<size_t>::max();
}

// ---- Registration through a staging buffer -----------------------

class RegistrationTest : public hipObjTest::FakeDeviceTest {};

TEST_F(RegistrationTest, GpuBufferFallsBackToStaging)
{
    ASSERT_NO_FATAL_FAILURE(init());
    fakeDeviceMemory();
    fake().fail.registerAddr = kStagedBuf;
    ASSERT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);

    /* The data goes through the staging buffer */
    EXPECT_EQ(hipObjBufSync(kStagedBuf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.memcpy, 1);
}

TEST_F(RegistrationTest, GrantsTheAccessTransfersNeed)
{
    /* The NIC writes the buffer for a GET (local write, and remote write
     * for the peer) and the peer reads it for a PUT */
    constexpr int kAccess = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    ASSERT_NO_FATAL_FAILURE(init());
    ASSERT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.mrAccess, kAccess);
    ASSERT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjSuccess);

    ASSERT_EQ(hipObjBufRegisterHost(kStagedBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.mrAccess, kAccess);
    ASSERT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjSuccess);

    /* The staging buffer gets the same access */
    fake().fail.registerAddr = kStagedBuf;
    ASSERT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.mrAccess, kAccess);
}

TEST_F(RegistrationTest, FailsWhenStagingBufferCannotBeAllocated)
{
    ASSERT_NO_FATAL_FAILURE(init());
    fake().fail.registerAddr = kStagedBuf;
    fake().fail.hostMalloc   = true;
    EXPECT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjRdmaError);
    EXPECT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjBufNotRegistered);
}

TEST_F(RegistrationTest, FailsWhenNothingCanBeRegistered)
{
    ASSERT_NO_FATAL_FAILURE(init());
    fake().fail.registerAll = true;
    EXPECT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjRdmaError);
    EXPECT_EQ(hipObjBufRegisterHost(kStagedBuf, kBufSize).opError, hipObjRdmaError);
    /* The device registration tried the caller's buffer and then the
     * staging buffer */
    EXPECT_EQ(fake().log.registerMr, 3);
    EXPECT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjBufNotRegistered);
}

TEST_F(RegistrationTest, LimitsNumberOfRegistrations)
{
    /* hipObject registers at most 256 buffers at a time */
    constexpr size_t kMaxBuffers = 256;
    ASSERT_NO_FATAL_FAILURE(init());
    const auto buffer = [](size_t i) { return hipObj::addressToPointer<void *>(0x100000 + (i * kBufSize)); };
    for (size_t i = 0; i < kMaxBuffers; ++i) {
        ASSERT_EQ(hipObjBufRegisterHost(buffer(i), kBufSize).opError, hipObjSuccess) << "buffer " << i;
    }
    EXPECT_EQ(hipObjBufRegisterHost(buffer(kMaxBuffers), kBufSize).opError, hipObjRdmaError);
    EXPECT_EQ(hipObjBufRegister(buffer(kMaxBuffers), kBufSize).opError, hipObjRdmaError);

    ASSERT_EQ(hipObjBufDeregister(buffer(0)).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufRegisterHost(buffer(kMaxBuffers), kBufSize).opError, hipObjSuccess);
}

// ---- Staging copies ----------------------------------------------

class StagingTest : public hipObjTest::FakeDeviceTest {
protected:
    /* Initializes with options, and registers kBufSize bytes at
     * kStagedBuf through a staging buffer */
    void initAndRegisterStaged(const hipObj::Options &options = {}) const
    {
        ASSERT_EQ(initFakes(makeConfig(), options).opError, hipObjSuccess);
        fake().fail.registerAddr = kStagedBuf;
        ASSERT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);
    }

    /* initAndRegisterStaged(), and a copy that times out, and doesn't
     * finish until the test sets fake().eventNeverCompletes to false */
    void initAndTimeOut() const
    {
        hipObj::Options options;
        options.stageTimeout = std::chrono::milliseconds(1);
        ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged(options));
        fake().eventNeverCompletes = true;
        ASSERT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
        ASSERT_EQ(fake().log.memcpy, 1);
    }

    static hipObjError_t sync(int direction)
    {
        return hipObjBufSync(kStagedBuf, kBufSize, 0, direction);
    }
};

TEST_F(StagingTest, WaitsForCopyToComplete)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    fake().eventNotReadyCount = 2;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    const hipObjTest::CallLog &log = fake().log;
    EXPECT_EQ(log.memcpy, 1);
    EXPECT_EQ(log.eventRecord, 1);
    EXPECT_EQ(log.eventQuery, 3);
    EXPECT_EQ(log.eventDestroy, 1);

    /* Each copy waits for its own event */
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(log.eventQuery, 6);
    EXPECT_EQ(log.eventDestroy, 2);
}

TEST_F(StagingTest, CopiesInTheRequestedDirection)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    ASSERT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(fake().lastMemcpyKind, hipMemcpyDeviceToHost);
    ASSERT_EQ(sync(HIPOBJ_SYNC_TO_DEVICE).opError, hipObjSuccess);
    EXPECT_EQ(fake().lastMemcpyKind, hipMemcpyHostToDevice);

    /* A GET copies the data it received to the device; a PUT copies the data
     * it sends from the device */
    hipObjOps_t ops = makeOps();
    ASSERT_EQ(hipObjGet(nullptr, kStagedBuf, kBufSize, 0, &ops, nullptr).opError, hipObjSuccess);
    EXPECT_EQ(fake().lastMemcpyKind, hipMemcpyHostToDevice);
    ASSERT_EQ(hipObjPut(nullptr, kStagedBuf, kBufSize, 0, &ops, nullptr).opError, hipObjSuccess);
    EXPECT_EQ(fake().lastMemcpyKind, hipMemcpyDeviceToHost);
}

TEST_F(StagingTest, CopiesOnTheCallingThreadsStream)
{
    /* Not the null stream, which would make copies from different threads
     * wait for each other */
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    ASSERT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(fake().lastMemcpyStream, hipStreamPerThread);
    EXPECT_EQ(fake().lastEventRecordStream, hipStreamPerThread);
}

TEST_F(StagingTest, FailsWithoutEvent)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    fake().eventCreateErr = hipErrorOutOfMemory;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_EQ(fake().log.memcpy, 0);
    EXPECT_EQ(fake().log.eventDestroy, 0);
}

TEST_F(StagingTest, ReleasesEventWhenCopyCannotStart)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    fake().memcpyErr = hipErrorInvalidValue;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_EQ(fake().log.eventRecord, 0);
    EXPECT_EQ(fake().log.eventDestroy, 1);

    fake().memcpyErr      = hipSuccess;
    fake().eventRecordErr = hipErrorInvalidValue;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_EQ(fake().log.eventQuery, 0);
    EXPECT_EQ(fake().log.eventDestroy, 2);
}

TEST_F(StagingTest, FailsAndReleasesEventWhenCopyFails)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    fake().eventNotReadyCount = 1;
    fake().eventQueryResult   = hipErrorLaunchFailure;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_EQ(fake().log.eventDestroy, 1);
}

// ---- Staging copies that time out --------------------------------

/* A copy that timed out may still use the staging buffer, so hipObject
 * keeps the buffer, and refuses to use it, until the copy is over */
TEST_F(StagingTest, TimedOutCopyKeepsTheBufferUntilItsOver)
{
    ASSERT_NO_FATAL_FAILURE(initAndTimeOut());

    hipObjOps_t ops   = makeOps();
    char       *token = nullptr;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_DEVICE).opError, hipObjBusy);
    EXPECT_EQ(hipObjGet(nullptr, kStagedBuf, kBufSize, 0, &ops, nullptr).opError, hipObjBusy);
    EXPECT_EQ(hipObjPut(nullptr, kStagedBuf, kBufSize, 0, &ops, nullptr).opError, hipObjBusy);
    EXPECT_EQ(hipObjGetRdmaToken(kStagedBuf, kBufSize, HIPOBJ_RDMA_OP_GET, &token).opError, hipObjBusy);
    EXPECT_EQ(token, nullptr);
    EXPECT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjBusy);

    /* Nothing started another copy or transfer, or released anything */
    EXPECT_EQ(fake().log.memcpy, 1);
    EXPECT_EQ(fake().log.sendRequest, 0);
    EXPECT_EQ(fake().log.eventDestroy, 0);
    EXPECT_EQ(fake().log.deregMr, 0);
    EXPECT_EQ(fake().log.hostFree, 0);

    /* Once the copy is over, its event is destroyed, and the buffer works
     * again */
    fake().eventNeverCompletes = false;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_DEVICE).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.memcpy, 2);
    EXPECT_EQ(fake().log.eventDestroy, 2);
    EXPECT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.hostFree, 1);
}

/* A copy that failed after it timed out is over too, and deregistering
 * releases it */
TEST_F(StagingTest, DeregisterReleasesACopyThatsOver)
{
    ASSERT_NO_FATAL_FAILURE(initAndTimeOut());
    fake().eventNeverCompletes = false;
    fake().eventQueryResult    = hipErrorLaunchFailure;

    EXPECT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.eventDestroy, 1);
    EXPECT_EQ(fake().log.deregMr, 1);
    EXPECT_EQ(fake().log.hostFree, 1);
}

/* Shutdown can't wait for a copy that never finishes, so it leaks the
 * staging buffer the copy uses, and releases everything else */
TEST_F(StagingTest, ShutdownLeaksTheStagingBufferOfACopyThatsRunning)
{
    ASSERT_NO_FATAL_FAILURE(initAndTimeOut());
    void *const stagingBuf = fake().lastHostMalloc;

    EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
    EXPECT_EQ(fake().log.hostFree, 0);
    EXPECT_EQ(fake().log.eventDestroy, 0);
    EXPECT_EQ(fake().log.deregMr, 1);
    EXPECT_EQ(fake().log.destroyQp, 1);
    EXPECT_EQ(fake().log.closeDevice, 1);

    /* Frees what the library leaked, which the fake allocated with
     * malloc(), so the leak checker doesn't report it */
    std::free(stagingBuf);
}

} // namespace
