/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Thread-safety tests for the public API. The functions that use library
 * state run one at a time, under one lock (ApiGuard, in state.h). These
 * tests call them from several threads at once and check that transfers
 * never overlap, that every call gets a result it could get on its own,
 * and that a callback that calls back into the library gets an error
 * instead of deadlocking. They run without GPU or RDMA hardware, through
 * the same seams as test-seams.cpp, and do the most good in a
 * ThreadSanitizer build, which reports any data race the lock misses.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <latch>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fake-device.h"
#include "fake-ibv.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "state.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

constexpr int    kThreads    = 8;
constexpr int    kIterations = 25;
constexpr size_t kBufSize    = 4096;

/* Runs body(index) on count threads, released together so their calls
 * overlap as much as possible, and returns when all of them are done */
void
runThreads(int count, const std::function<void(int)> &body)
{
    std::latch                start(count);
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&start, &body, i] {
            start.arrive_and_wait();
            body(i);
        });
    }
    // Each std::jthread joins its thread when it's destroyed
}

// ---- Transfer callbacks ------------------------------------------------

int
plainSendRequest(void *, const char *token, size_t tokenLen)
{
    hipObjTest::noteSentToken(token, tokenLen);
    return 0;
}

int
plainRecvReply(void *, char *reply, size_t *replyLen)
{
#ifdef HIPOBJ_RDMA_WIRE_V1
    static constexpr std::string_view kReply = "200";
#else
    // v3 wire format (see v3-wire.h): the fake device's success reply, which
    // reports the length the token named, as the real server does
    const std::string kReply = hipObjTest::successReply();
#endif
    if (!reply || !replyLen || *replyLen < kReply.size()) {
        return -1;
    }
    std::ranges::copy(kReply, hipObj::spanFromPointer(reply, *replyLen).begin());
    *replyLen = kReply.size();
    return 0;
}

/* Watches transfers for overlap. A transfer is active from its
 * sendRequest callback to its recvReply callback. active and overlaps are
 * atomic, so overlap is counted even if the lock is missing. transfers
 * deliberately isn't: if two transfers ever run at once, ThreadSanitizer
 * reports a data race on it. */
struct TransferProbe {
    std::atomic<int> active{0};
    std::atomic<int> overlaps{0};
    int              transfers = 0;
};

int
probeSendRequest(void *ctx, const char *token, size_t tokenLen)
{
    hipObjTest::noteSentToken(token, tokenLen);
    auto *probe = static_cast<TransferProbe *>(ctx);
    if (probe->active.fetch_add(1) != 0) {
        probe->overlaps.fetch_add(1);
    }
    ++probe->transfers;
    /* Holds the transfer open long enough that, without the lock, other
     * threads' transfers would start inside it */
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    return 0;
}

int
probeRecvReply(void *ctx, char *reply, size_t *replyLen)
{
    auto *probe = static_cast<TransferProbe *>(ctx);
    probe->active.fetch_sub(1);
    return plainRecvReply(ctx, reply, replyLen);
}

hipObjConfig_t
makeConfig()
{
    hipObjConfig_t config{};
    config.gpuDevice = -1;
    config.nicHint   = "mlx5_0";
    return config;
}

/* Installs the fakes for each test, and puts the real ones back
 * afterward */
class ThreadSafetyTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        /* The fake HIP runtime has no GPU, so with a NIC hint,
         * hipObjInit() opens the hinted NIC directly */
        fakeHip_.emplace(hip_);

        fakeIbv_.emplace(verbs_);
        fakeRcListener_.emplace(rcListener_);
#ifndef HIPOBJ_RDMA_WIRE_V1
        /* v3's listener binds to the NIC's address */
        hipDevice_.gid = hipObjTest::kFakeNicGid;
#endif

        probeOps_.sendRequest = &probeSendRequest;
        probeOps_.recvReply   = &probeRecvReply;
    }

    void TearDown() override
    {
        (void)hipObjShutdown();
        fakeRcListener_.reset();
        fakeIbv_.reset();
        fakeHip_.reset();
    }

    /* The buffer for thread index, kBufSize bytes, distinct from every
     * other thread's */
    void *buffer(int index)
    {
        return std::span(pool_).subspan(static_cast<size_t>(index) * kBufSize, kBufSize).data();
    }

    hipObjTest::FakeDevice                          hipDevice_{};
    hipObjTest::FakeVerbs                           verbs_{hipDevice_};
    std::optional<hipObjTest::ScopedFakeIbv>        fakeIbv_{};
    hipObjTest::FakeHip                             hip_{hipDevice_};
    std::optional<hipObjTest::ScopedFakeHip>        fakeHip_{};
    hipObjTest::FakeRcListener                      rcListener_{hipDevice_};
    std::optional<hipObjTest::ScopedFakeRcListener> fakeRcListener_{};
    std::vector<char>                               pool_ = std::vector<char>(kThreads * kBufSize);
    TransferProbe                                   probe_{};
    hipObjOps_t                                     probeOps_{};
};

TEST_F(ThreadSafetyTest, TransfersRunOneAtATime)
{
    hipObjConfig_t config = makeConfig();
    ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_EQ(hipObjBufRegisterHost(buffer(t), kBufSize).opError, hipObjSuccess);
    }

    runThreads(kThreads, [this](int t) {
        for (int i = 0; i < kIterations; ++i) {
            hipObjError_t err = (i % 2 == 0)
                                    ? hipObjGet(nullptr, buffer(t), kBufSize, 0, &probeOps_, &probe_)
                                    : hipObjPut(nullptr, buffer(t), kBufSize, 0, &probeOps_, &probe_);
            EXPECT_EQ(err.opError, hipObjSuccess);
        }
    });

    EXPECT_EQ(probe_.overlaps.load(), 0);
    EXPECT_EQ(probe_.transfers, kThreads * kIterations);
}

TEST_F(ThreadSafetyTest, BufferCallsFromManyThreads)
{
    hipObjConfig_t config = makeConfig();
    ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

    /* Each thread works on its own buffer, so every call succeeds, while
     * the other threads' calls change the buffer map under it */
    runThreads(kThreads, [this](int t) {
        void *buf = buffer(t);
        for (int i = 0; i < kIterations; ++i) {
            EXPECT_EQ(hipObjBufRegisterHost(buf, kBufSize).opError, hipObjSuccess);
            EXPECT_EQ(hipObjGet(nullptr, buf, kBufSize, 0, &probeOps_, &probe_).opError, hipObjSuccess);
            EXPECT_EQ(hipObjBufSync(buf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
            EXPECT_EQ(hipObjPut(nullptr, buf, kBufSize, 0, &probeOps_, &probe_).opError, hipObjSuccess);
            EXPECT_EQ(hipObjBufSync(buf, kBufSize, 0, HIPOBJ_SYNC_TO_DEVICE).opError, hipObjSuccess);

            char *token = nullptr;
            EXPECT_EQ(hipObjGetRdmaToken(buf, kBufSize, HIPOBJ_RDMA_OP_GET, &token).opError, hipObjSuccess);
            if (token) {
                EXPECT_EQ(hipObjPutRdmaToken(token).opError, hipObjSuccess);
            }

            EXPECT_EQ(hipObjBufDeregister(buf).opError, hipObjSuccess);
        }
    });

    EXPECT_EQ(probe_.overlaps.load(), 0);
    EXPECT_EQ(probe_.transfers, 2 * kThreads * kIterations);
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(hipObjBufDeregister(buffer(t)).opError, hipObjBufNotRegistered);
    }
}

TEST_F(ThreadSafetyTest, ConcurrentInitHasOneWinner)
{
    std::atomic<int> succeeded{0};
    std::atomic<int> alreadyInitialized{0};

    runThreads(kThreads, [&](int) {
        hipObjConfig_t  config = makeConfig();
        hipObjOpError_t err    = hipObjInit(&config).opError;
        if (err == hipObjSuccess) {
            succeeded.fetch_add(1);
        }
        else if (err == hipObjAlreadyInitialized) {
            alreadyInitialized.fetch_add(1);
        }
        else {
            ADD_FAILURE() << "hipObjInit() returned " << err;
        }
    });

    EXPECT_EQ(succeeded.load(), 1);
    EXPECT_EQ(alreadyInitialized.load(), kThreads - 1);
    EXPECT_EQ(hipDevice_.log.openDevice, 1);
}

TEST_F(ThreadSafetyTest, ShutdownAndInitDuringBufferCalls)
{
    hipObjConfig_t config = makeConfig();
    ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

    /* The results a call can get when the library may be shut down, and
     * all buffers deregistered, before or after it */
    auto possible = [](hipObjOpError_t err) {
        return err == hipObjSuccess || err == hipObjNotInitialized || err == hipObjBufNotRegistered;
    };

    runThreads(kThreads, [&](int t) {
        if (t == 0) {
            /* Only this thread shuts down and initializes, so neither fails */
            for (int i = 0; i < kIterations; ++i) {
                EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
                hipObjConfig_t again = makeConfig();
                EXPECT_EQ(hipObjInit(&again).opError, hipObjSuccess);
            }
            return;
        }
        void *buf = buffer(t);
        for (int i = 0; i < kIterations; ++i) {
            EXPECT_PRED1(possible, hipObjBufRegisterHost(buf, kBufSize).opError);
            EXPECT_PRED1(possible, hipObjGet(nullptr, buf, kBufSize, 0, &probeOps_, &probe_).opError);
            EXPECT_PRED1(possible, hipObjBufDeregister(buf).opError);
        }
    });

    EXPECT_EQ(probe_.overlaps.load(), 0);
    EXPECT_NE(hipObj::currentInstance(), nullptr);
}

/* The number of functions reentrantSendRequest() calls that take the
 * API lock */
constexpr size_t kLockedFunctions = 9;

/* What reentrantSendRequest() needs, and what it found */
struct ReentryProbe {
    void       *registered   = nullptr;
    void       *unregistered = nullptr;
    hipObjOps_t plainOps     = {&plainSendRequest, &plainRecvReply};

    std::vector<std::pair<std::string_view, hipObjOpError_t>> locked{};
    char                                                     *token       = nullptr;
    hipObjOpError_t                                           parseResult = hipObjInternalError;
    int                                                       parsedCode  = 0;
};

/* Calls every function that takes the API lock, with arguments that would
 * make each one succeed or change something if it ran, and then one that
 * doesn't take the lock */
int
reentrantSendRequest(void *ctx, const char *token, size_t tokenLen)
{
    auto          *probe  = static_cast<ReentryProbe *>(ctx);
    hipObjConfig_t config = makeConfig();
    hipObjTest::noteSentToken(token, tokenLen);

    probe->locked = {
        {"hipObjInit", hipObjInit(&config).opError},
        {"hipObjShutdown", hipObjShutdown().opError},
        {"hipObjBufRegister", hipObjBufRegister(probe->unregistered, kBufSize).opError},
        {"hipObjBufRegisterHost", hipObjBufRegisterHost(probe->unregistered, kBufSize).opError},
        {"hipObjBufDeregister", hipObjBufDeregister(probe->registered).opError},
        {"hipObjGet", hipObjGet(nullptr, probe->registered, kBufSize, 0, &probe->plainOps, nullptr).opError},
        {"hipObjPut", hipObjPut(nullptr, probe->registered, kBufSize, 0, &probe->plainOps, nullptr).opError},
        {"hipObjBufSync", hipObjBufSync(probe->registered, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError},
        {"hipObjGetRdmaToken",
         hipObjGetRdmaToken(probe->registered, kBufSize, HIPOBJ_RDMA_OP_GET, &probe->token).opError},
    };

    static constexpr std::string_view kReply = "200";
    probe->parseResult = hipObjParseRdmaReply(kReply.data(), kReply.size(), &probe->parsedCode).opError;
    return 0;
}

TEST_F(ThreadSafetyTest, CallbackReentryFailsInsteadOfDeadlocking)
{
    hipObjConfig_t config = makeConfig();
    ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

    ReentryProbe probe;
    probe.registered   = buffer(0);
    probe.unregistered = buffer(1);
    ASSERT_EQ(hipObjBufRegisterHost(probe.registered, kBufSize).opError, hipObjSuccess);

    hipObjOps_t ops = {&reentrantSendRequest, &plainRecvReply};
    EXPECT_EQ(hipObjGet(nullptr, probe.registered, kBufSize, 0, &ops, &probe).opError, hipObjSuccess);

    EXPECT_EQ(probe.locked.size(), kLockedFunctions);
    for (const auto &[name, err] : probe.locked) {
        EXPECT_EQ(err, hipObjInvalidValue) << name << " from a callback";
    }
    EXPECT_EQ(probe.token, nullptr);
    if (probe.token) {
        (void)hipObjPutRdmaToken(probe.token);
    }

    /* Functions that don't take the lock work from a callback */
    EXPECT_EQ(probe.parseResult, hipObjSuccess);
    EXPECT_EQ(probe.parsedCode, 200);

    /* The rejected calls changed nothing, and the lock was released */
    EXPECT_NE(hipObj::currentInstance(), nullptr);
    EXPECT_EQ(hipObjBufDeregister(probe.unregistered).opError, hipObjBufNotRegistered);
    EXPECT_EQ(hipObjBufDeregister(probe.registered).opError, hipObjSuccess);
}

TEST_F(ThreadSafetyTest, StatelessFunctionsFromManyThreads)
{
    hipObjConfig_t config = makeConfig();
    ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);
    ASSERT_EQ(hipObjBufRegisterHost(buffer(0), kBufSize).opError, hipObjSuccess);
    char *token = nullptr;
    ASSERT_EQ(hipObjGetRdmaToken(buffer(0), kBufSize, HIPOBJ_RDMA_OP_GET, &token).opError, hipObjSuccess);
    /* Releases the token however the test ends */
    std::unique_ptr<char, decltype(&hipObjPutRdmaToken)> tokenOwner(token, &hipObjPutRdmaToken);

    const std::string expectedVersion = std::to_string(HIPOBJ_VERSION_MAJOR) + "." +
                                        std::to_string(HIPOBJ_VERSION_MINOR) + "." +
                                        std::to_string(HIPOBJ_VERSION_PATCH);
    std::array<const char *, kThreads> versions{};

    runThreads(kThreads, [&](int t) {
        versions[static_cast<size_t>(t)] = hipObjGetVersionString();
        for (int i = 0; i < kIterations; ++i) {
            EXPECT_STREQ(hipObjGetErrorString(hipObjBufNotRegistered), "Buffer not registered");

            static constexpr std::string_view kReply = "206";
            int                               code   = 0;
            EXPECT_EQ(hipObjParseRdmaReply(kReply.data(), kReply.size(), &code).opError, hipObjSuccess);
            EXPECT_EQ(code, 206);

#ifdef HIPOBJ_RDMA_WIRE_V1
            /* Only v1 tokens carry the client NIC */
            std::array<char, 32> nicIp{};
            EXPECT_EQ(hipObjTokenClientNic(token, nicIp.data(), nicIp.size()).opError, hipObjSuccess);
#endif
        }
    });

    for (const char *version : versions) {
        ASSERT_NE(version, nullptr);
        EXPECT_EQ(version, versions[0]);
        EXPECT_STREQ(version, expectedVersion.c_str());
    }
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
