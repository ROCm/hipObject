/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Buffers the NIC can't reach directly: registering them through a host
 * staging buffer, and copying between the staging buffer and the caller's
 * memory with a deadline, against the fake device in fake-device.h.
 *
 * The StagingEnvTest tests need HIPOBJ_STAGE_TIMEOUT_MS and
 * HIPOBJ_REQUIRE_GPU_DIRECT, which hipObject reads once and caches, so they
 * skip unless they're set. CMakeLists.txt runs this program a second time,
 * with both set, for just those tests, and a third time with a malformed
 * HIPOBJ_STAGE_TIMEOUT_MS for the StagingBadEnvTest tests.
 */

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "fake-device.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::makeOps;

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

/* reg_mr fails for this address, so it's registered through a staging
 * buffer */
void *const kStagedBuf = reinterpret_cast<void *>(0x30000);

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
    auto buffer = [](size_t i) { return reinterpret_cast<void *>(0x100000 + i * kBufSize); };
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
    /* Initializes and registers kBufSize bytes at kStagedBuf through a
     * staging buffer */
    static void initAndRegisterStaged()
    {
        ASSERT_NO_FATAL_FAILURE(init());
        fake().fail.registerAddr = kStagedBuf;
        ASSERT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);
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

// ---- Settings read from the environment --------------------------

class StagingEnvTest : public StagingTest {
protected:
    void SetUp() override
    {
        if (!std::getenv("HIPOBJ_STAGE_TIMEOUT_MS") || !std::getenv("HIPOBJ_REQUIRE_GPU_DIRECT")) {
            GTEST_SKIP() << "needs HIPOBJ_STAGE_TIMEOUT_MS and HIPOBJ_REQUIRE_GPU_DIRECT";
        }
        StagingTest::SetUp();
    }
};

TEST_F(StagingEnvTest, AbandonsCopyThatMissesDeadline)
{
    const char *envValue = std::getenv("HIPOBJ_STAGE_TIMEOUT_MS");
    ASSERT_NE(envValue, nullptr);
    const std::string_view env       = envValue;
    int64_t                timeoutMs = 0;
    ASSERT_EQ(std::from_chars(env.data(), std::to_address(env.end()), timeoutMs).ec, std::errc{});
    ASSERT_GT(timeoutMs, 0);
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    fake().eventNeverCompletes = true;

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(timeoutMs));

    /* The copy may still land, so the event that tracks it isn't destroyed */
    EXPECT_EQ(fake().log.eventDestroy, 0);
}

TEST_F(StagingEnvTest, RequiringGpuDirectRefusesStaging)
{
    ASSERT_NO_FATAL_FAILURE(init());
    fakeDeviceMemory();
    fake().fail.registerAddr = kStagedBuf;
    EXPECT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.registerMr, 1);
    EXPECT_EQ(hipObjBufDeregister(kStagedBuf).opError, hipObjBufNotRegistered);

    /* Host memory isn't GPU-direct to begin with */
    fake().memoryType = hipMemoryTypeHost;
    EXPECT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);
}

/* For a HIPOBJ_STAGE_TIMEOUT_MS that isn't a plain number, such as "+20" */
class StagingBadEnvTest : public StagingTest {
protected:
    void SetUp() override
    {
        const char *env = std::getenv("HIPOBJ_STAGE_TIMEOUT_MS");
        if (!env || std::string_view(env).find_first_not_of("0123456789") == std::string_view::npos) {
            GTEST_SKIP() << "needs a malformed HIPOBJ_STAGE_TIMEOUT_MS";
        }
        StagingTest::SetUp();
    }
};

TEST_F(StagingBadEnvTest, IgnoresMalformedTimeout)
{
    ASSERT_NO_FATAL_FAILURE(initAndRegisterStaged());
    /* At least 100 ms, since hipObject sleeps 1 ms between polls. That's
     * well under the default timeout, and over the 20 ms that "+20" would
     * mean if it weren't ignored. */
    fake().eventNotReadyCount = 100;
    EXPECT_EQ(sync(HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(fake().log.eventDestroy, 1);
}

} // namespace
