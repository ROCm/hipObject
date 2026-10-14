/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * The library's root: parsing the options from the environment, and
 * building, publishing, and destroying the instance with initWith() and
 * hipObjShutdown(), against the fake device in fake-device.h.
 */

#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "buffer.h"
#include "fake-device.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "state.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::makeOps;
using std::chrono::milliseconds;

/* An environment, by variable name */
using Environment = std::map<std::string, std::string, std::less<>>;

hipObj::Options
parse(const Environment &env)
{
    return hipObj::parseOptions([&env](const char *name) -> const char * {
        const auto it = env.find(std::string_view(name));
        return it == env.end() ? nullptr : it->second.c_str();
    });
}

// ---- parseOptions ------------------------------------------------

TEST(ParseOptionsTest, DefaultsWhenNothingIsSet)
{
    const hipObj::Options options = parse({});
    EXPECT_EQ(options.stageTimeout, std::chrono::seconds(30));
    EXPECT_EQ(options.pollTimeout, std::chrono::seconds(5));
    EXPECT_FALSE(options.requireGpuDirect);
    EXPECT_TRUE(options.dmabuf.enabled);
    EXPECT_FALSE(options.dmabuf.debug);
}

TEST(ParseOptionsTest, StageTimeout)
{
    EXPECT_EQ(parse({{"HIPOBJ_STAGE_TIMEOUT_MS", "20"}}).stageTimeout, milliseconds(20));
    EXPECT_EQ(parse({{"HIPOBJ_STAGE_TIMEOUT_MS", "1"}}).stageTimeout, milliseconds(1));
    EXPECT_EQ(parse({{"HIPOBJ_STAGE_TIMEOUT_MS", "86400000"}}).stageTimeout, std::chrono::hours(24));
}

TEST(ParseOptionsTest, InvalidStageTimeoutIsIgnored)
{
    for (const char *value :
         {"", "0", "-20", "+20", " 20", "20 ", "20ms", "0x14", "86400001", "99999999999999999999"}) {
        EXPECT_EQ(parse({{"HIPOBJ_STAGE_TIMEOUT_MS", value}}).stageTimeout, std::chrono::seconds(30))
            << "\"" << value << "\"";
    }
}

TEST(ParseOptionsTest, RequireGpuDirect)
{
    for (const char *value : {"1", "yes", "Y", "true"}) {
        EXPECT_TRUE(parse({{"HIPOBJ_REQUIRE_GPU_DIRECT", value}}).requireGpuDirect) << "\"" << value << "\"";
    }
    for (const char *value : {"", "0", "no", "No", "n", "N"}) {
        EXPECT_FALSE(parse({{"HIPOBJ_REQUIRE_GPU_DIRECT", value}}).requireGpuDirect) << "\"" << value << "\"";
    }
}

TEST(ParseOptionsTest, Dmabuf)
{
    for (const char *value : {"", "1", "yes"}) {
        EXPECT_TRUE(parse({{"HIPOBJ_DMABUF", value}}).dmabuf.enabled) << "\"" << value << "\"";
    }
    for (const char *value : {"0", "no", "No", "n", "N"}) {
        EXPECT_FALSE(parse({{"HIPOBJ_DMABUF", value}}).dmabuf.enabled) << "\"" << value << "\"";
    }
}

TEST(ParseOptionsTest, RdmaDebug)
{
    for (const char *value : {"1", "yes"}) {
        EXPECT_TRUE(parse({{"HIPOBJ_RDMA_DEBUG", value}}).dmabuf.debug) << "\"" << value << "\"";
    }
    for (const char *value : {"", "0"}) {
        EXPECT_FALSE(parse({{"HIPOBJ_RDMA_DEBUG", value}}).dmabuf.debug) << "\"" << value << "\"";
    }
}

/* Each variable sets only its own option */
TEST(ParseOptionsTest, AllSet)
{
    const hipObj::Options options = parse({
        {"HIPOBJ_STAGE_TIMEOUT_MS", "250"},
        {"HIPOBJ_REQUIRE_GPU_DIRECT", "1"},
        {"HIPOBJ_DMABUF", "0"},
        {"HIPOBJ_RDMA_DEBUG", "1"},
    });
    EXPECT_EQ(options.stageTimeout, milliseconds(250));
    EXPECT_EQ(options.pollTimeout, std::chrono::seconds(5));
    EXPECT_TRUE(options.requireGpuDirect);
    EXPECT_FALSE(options.dmabuf.enabled);
    EXPECT_TRUE(options.dmabuf.debug);
}

// ---- initWith ----------------------------------------------------

/* reg_mr fails for this address, so it's registered through a staging
 * buffer */
void *const kStagedBuf = reinterpret_cast<void *>(0x30000);

constexpr size_t kBufSize = hipObjTest::kFakeBufSize;

class InitWithTest : public hipObjTest::FakeDeviceTest {
protected:
    /* initWith() with the system platform, which is the fakes, and
     * options, holding the API lock as hipObjInit() does */
    static hipObjError_t initWith(std::shared_ptr<const hipObj::Platform> platform,
                                  const hipObj::Options                  &options)
    {
        const hipObj::ApiGuard guard;
        const hipObjConfig_t   config = makeConfig();
        return hipObj::initWith(config, std::move(platform), options);
    }

    static hipObjError_t initWith(const hipObj::Options &options)
    {
        return initWith(hipObj::systemPlatform(), options);
    }
};

TEST_F(InitWithTest, InstanceKeepsItsPlatformAndOptions)
{
    const std::shared_ptr<const hipObj::Platform> platform = hipObj::systemPlatform();
    hipObj::Options                               options;
    options.stageTimeout     = milliseconds(7);
    options.pollTimeout      = milliseconds(9);
    options.requireGpuDirect = true;
    ASSERT_EQ(initWith(platform, options).opError, hipObjSuccess);

    {
        const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
        ASSERT_NE(instance, nullptr);
        EXPECT_EQ(instance->platform, platform);
        EXPECT_EQ(instance->options.stageTimeout, milliseconds(7));
        EXPECT_EQ(instance->options.pollTimeout, milliseconds(9));
        EXPECT_TRUE(instance->options.requireGpuDirect);
        EXPECT_EQ(instance->gpuDevice, -1);
        EXPECT_EQ(instance->nicHint, "mlx5_0");
        EXPECT_EQ(instance->nicIndex, 0);
    }

    /* Shutting down releases the instance's copy of the platform */
    EXPECT_EQ(platform.use_count(), 2);
    ASSERT_EQ(hipObjShutdown().opError, hipObjSuccess);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    EXPECT_EQ(platform.use_count(), 1);
}

TEST_F(InitWithTest, FailureLeavesNoInstance)
{
    const std::shared_ptr<const hipObj::Platform> platform = hipObj::systemPlatform();
    fake().fail.createQp                                   = true;
    EXPECT_EQ(initWith(platform, {}).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    EXPECT_EQ(platform.use_count(), 1);

    /* The device opened before the failure was closed */
    EXPECT_EQ(fake().log.openDevice, 1);
    EXPECT_EQ(fake().log.closeDevice, 1);
}

TEST_F(InitWithTest, AlreadyInitialized)
{
    ASSERT_NO_FATAL_FAILURE(init());
    const std::shared_ptr<hipObj::Instance> instance = hipObj::currentInstance();
    EXPECT_EQ(initWith({}).opError, hipObjAlreadyInitialized);
    EXPECT_EQ(hipObj::currentInstance(), instance);
    EXPECT_EQ(fake().log.openDevice, 1);
}

TEST_F(InitWithTest, PollTimeoutLimitsTheWaitForACompletion)
{
    hipObj::Options options;
    options.pollTimeout = milliseconds(10);
    ASSERT_EQ(initWith(options).opError, hipObjSuccess);
    ASSERT_EQ(hipObjBufRegister(hipObjTest::fakeDevBuf(), kBufSize).opError, hipObjSuccess);

    /* Nothing completes, so the transfer fails once the timeout passes,
     * well before the default one would */
    fake().pollResult  = 0;
    hipObjOps_t ops    = makeOps();
    const auto  start  = std::chrono::steady_clock::now();
    const auto  result = hipObjGet(nullptr, hipObjTest::fakeDevBuf(), kBufSize, 0, &ops, nullptr);
    EXPECT_EQ(result.opError, hipObjRdmaError);
    EXPECT_LT(std::chrono::steady_clock::now() - start, hipObj::Options{}.pollTimeout);
}

TEST_F(InitWithTest, StageTimeoutLimitsTheWaitForACopy)
{
    hipObj::Options options;
    options.stageTimeout = milliseconds(20);
    ASSERT_EQ(initWith(options).opError, hipObjSuccess);
    fake().fail.registerAddr = kStagedBuf;
    ASSERT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjSuccess);

    fake().eventNeverCompletes = true;
    const auto start           = std::chrono::steady_clock::now();
    EXPECT_EQ(hipObjBufSync(kStagedBuf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjInternalError);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, options.stageTimeout);
    EXPECT_LT(elapsed, hipObj::Options{}.stageTimeout);
}

TEST_F(InitWithTest, RequireGpuDirectRefusesStaging)
{
    hipObj::Options options;
    options.requireGpuDirect = true;
    ASSERT_EQ(initWith(options).opError, hipObjSuccess);

    /* GPU memory the NIC can't register */
    fake().memoryType        = hipMemoryTypeDevice;
    fake().allocBase         = 0;
    fake().allocSize         = 0x100000;
    fake().fail.registerAddr = kStagedBuf;
    EXPECT_EQ(hipObjBufRegister(kStagedBuf, kBufSize).opError, hipObjRdmaError);
    EXPECT_EQ(fake().log.hostMalloc, 0);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
