/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Thread-safety tests for the public API. There's no API lock: calls run
 * at the same time, and each transfer leases a connection of its own.
 * These tests call the library from several threads at once, and from
 * callbacks, and check that transfers run concurrently and wait for a
 * connection when every one is leased, that a buffer in use can't be
 * deregistered, that shutdown leaves the instance to the calls still
 * using it, and that every call gets a result it could get on its own.
 * They run without GPU or RDMA hardware, through the same seams as
 * test-seams.cpp, and do the most good in a ThreadSanitizer build, which
 * reports any data race the locks miss.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fake-device.h"
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

/* How long a test waits for something it expects to happen, so that a
 * regression fails the test instead of hanging it */
constexpr auto kTimeout = std::chrono::seconds(10);

/* How long a test waits to see that a thread is still waiting */
constexpr auto kStillWaiting = std::chrono::milliseconds(50);

/* Starts body(index) on count threads, released together so their calls
 * overlap as much as possible. Each std::jthread joins its thread when
 * it's destroyed. The threads can outlive the call, so each one has its
 * own copy of body, and shares the latch. */
std::vector<std::jthread>
startThreads(int count, const std::function<void(int)> &body)
{
    const auto                start = std::make_shared<std::latch>(count);
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([start, body, i] {
            start->arrive_and_wait();
            body(i);
        });
    }
    return threads;
}

/* startThreads(), returning when all of the threads are done */
void
runThreads(int count, const std::function<void(int)> &body)
{
    startThreads(count, body);
}

// ---- Transfer callbacks ------------------------------------------------

int
plainSendRequest(void *, const char *token, size_t tokenLen)
{
    hipObjTest::noteSentToken(token, tokenLen);
    return 0;
}

int
plainRecvReply(void * /*ctx*/, char *reply, size_t *replyLen)
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

/* Counts transfers in an std::atomic<int>, their context */
int
countingSendRequest(void *ctx, const char * /*token*/, size_t /*tokenLen*/)
{
    static_cast<std::atomic<int> *>(ctx)->fetch_add(1);
    return 0;
}

/* Holds transfers in their sendRequest callback until the test lets them
 * go, and records what they did. A transfer that isn't let go within
 * kTimeout fails, so a test whose transfers don't run as it expects
 * fails instead of hanging. */
class TransferGate {
public:
    /* sendRequest, with token: records the token's QP number, and waits
     * to be let go */
    int enter(std::string_view token)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (const auto decoded = hipObj::decodeRdmaTokenHex(token)) {
            qpNums_.insert(decoded->qpNum);
        }
        repliedAtEntry_.push_back(replied_);
        changed_.notify_all();
        if (!changed_.wait_for(lock, kTimeout, [this] { return released_ > 0; })) {
            return -1;
        }
        --released_;
        return 0;
    }

    /* recvReply */
    void replied()
    {
        const std::scoped_lock<std::mutex> lock(mutex_);
        ++replied_;
    }

    /* Lets count transfers go, now or when they enter */
    void release(int count)
    {
        {
            const std::scoped_lock<std::mutex> lock(mutex_);
            released_ += count;
        }
        changed_.notify_all();
    }

    /* Waits until count transfers have entered. False if they don't
     * within kTimeout. */
    bool waitForEntered(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kTimeout, [&] { return repliedAtEntry_.size() >= count; });
    }

    [[nodiscard]] size_t entered() const
    {
        const std::scoped_lock<std::mutex> lock(mutex_);
        return repliedAtEntry_.size();
    }

    /* The QP numbers in the tokens of the transfers that entered */
    [[nodiscard]] std::set<uint32_t> qpNums() const
    {
        const std::scoped_lock<std::mutex> lock(mutex_);
        return qpNums_;
    }

    /* How many transfers had received their reply when each transfer
     * entered, in the order they entered */
    [[nodiscard]] std::vector<int> repliedAtEntry() const
    {
        const std::scoped_lock<std::mutex> lock(mutex_);
        return repliedAtEntry_;
    }

private:
    mutable std::mutex      mutex_{};
    std::condition_variable changed_{};
    std::set<uint32_t>      qpNums_{};
    std::vector<int>        repliedAtEntry_{};
    int                     replied_  = 0;
    int                     released_ = 0;
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
gateRecvReply(void *ctx, char *reply, size_t *replyLen)
{
    static_cast<TransferGate *>(ctx)->replied();
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

/* Options with maxTransfers connections for transfers */
hipObj::Options
withMaxTransfers(size_t maxTransfers)
{
    hipObj::Options options;
    options.maxTransfers = maxTransfers;
    return options;
}

/* Shuts the library down after each test, and checks that nothing but the
 * fixture still holds the fakes */
class ThreadSafetyTest : public ::testing::Test {
protected:
    void TearDown() override
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

    /* Initializes the library with the fakes. The fake HIP runtime has no
     * GPU, so with a NIC hint, initialization opens the hinted NIC
     * directly. */
    [[nodiscard]] hipObjOpError_t init(const hipObj::Options &options = {}) const
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

// ---- Transfers ---------------------------------------------------------

/* Every transfer is in its sendRequest callback at once, which they can
 * only be if they run at the same time, and each one's token names a
 * different QP */
TEST_F(ThreadSafetyTest, TransfersRunConcurrently)
{
    ASSERT_EQ(init(), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(kThreads));

    TransferGate                          gate;
    std::array<hipObjOpError_t, kThreads> results{};
    {
        const auto running = startThreads(kThreads, [&](int t) {
            results.at(static_cast<size_t>(t)) =
                (t % 2 == 0) ? hipObjGet(nullptr, buffer(t), kBufSize, 0, &gateOps_, &gate).opError
                             : hipObjPut(nullptr, buffer(t), kBufSize, 0, &gateOps_, &gate).opError;
        });
        EXPECT_TRUE(gate.waitForEntered(static_cast<size_t>(kThreads)));
        gate.release(kThreads);
    }

    for (const hipObjOpError_t err : results) {
        EXPECT_EQ(err, hipObjSuccess);
    }
    EXPECT_EQ(gate.qpNums().size(), static_cast<size_t>(kThreads));
}

/* With two connections, a third transfer waits until one of the first two
 * has finished */
TEST_F(ThreadSafetyTest, TransferWaitsForAConnectionWhenAllAreLeased)
{
    constexpr int kTransfers = 3;
    ASSERT_EQ(init(withMaxTransfers(2)), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(kTransfers));

    TransferGate                            gate;
    std::array<hipObjOpError_t, kTransfers> results{};
    {
        const auto running = startThreads(kTransfers, [&](int t) {
            results.at(static_cast<size_t>(t)) =
                hipObjGet(nullptr, buffer(t), kBufSize, 0, &gateOps_, &gate).opError;
        });
        EXPECT_TRUE(gate.waitForEntered(2));
        std::this_thread::sleep_for(kStillWaiting);
        EXPECT_EQ(gate.entered(), 2U);
        EXPECT_EQ(hipObj::currentInstance()->pool.available(), 0U);

        gate.release(1);
        EXPECT_TRUE(gate.waitForEntered(static_cast<size_t>(kTransfers)));
        gate.release(kTransfers - 1);
    }

    for (const hipObjOpError_t err : results) {
        EXPECT_EQ(err, hipObjSuccess);
    }
    const std::vector<int> replied = gate.repliedAtEntry();
    ASSERT_EQ(replied.size(), static_cast<size_t>(kTransfers));
    EXPECT_GE(replied.back(), 1);
}

/* What nestedSendRequest() needs, and what it got */
struct NestedProbe {
    void           *buffer   = nullptr;
    hipObjOps_t     plainOps = {.sendRequest = &plainSendRequest, .recvReply = &plainRecvReply};
    hipObjOpError_t result   = hipObjInternalError;
};

/* Starts a transfer from inside a transfer */
int
nestedSendRequest(void *ctx, const char * /*token*/, size_t /*tokenLen*/)
{
    auto *probe   = static_cast<NestedProbe *>(ctx);
    probe->result = hipObjGet(nullptr, probe->buffer, kBufSize, 0, &probe->plainOps, nullptr).opError;
    return 0;
}

/* A callback's transfer doesn't wait for the connection its own transfer
 * holds */
TEST_F(ThreadSafetyTest, TransferFromACallbackIsBusyWhenNoConnectionIsFree)
{
    ASSERT_EQ(init(withMaxTransfers(1)), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(2));

    NestedProbe probe;
    probe.buffer    = buffer(1);
    hipObjOps_t ops = {.sendRequest = &nestedSendRequest, .recvReply = &plainRecvReply};
    EXPECT_EQ(hipObjGet(nullptr, buffer(0), kBufSize, 0, &ops, &probe).opError, hipObjSuccess);
    EXPECT_EQ(probe.result, hipObjBusy);

    /* The outer transfer returned the connection */
    EXPECT_EQ(hipObjGet(nullptr, buffer(1), kBufSize, 0, &plainOps_, nullptr).opError, hipObjSuccess);
}

/* A transfer waiting for a connection stops waiting when the library shuts
 * down, and the transfer holding the connection finishes */
TEST_F(ThreadSafetyTest, ShutdownStopsATransferWaitingForAConnection)
{
    ASSERT_EQ(init(withMaxTransfers(1)), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(2));

    TransferGate    gate;
    hipObjOpError_t holding = hipObjInternalError;
    hipObjOpError_t waiting = hipObjInternalError;
    {
        const std::jthread holder(
            [&] { holding = hipObjGet(nullptr, buffer(0), kBufSize, 0, &gateOps_, &gate).opError; });
        EXPECT_TRUE(gate.waitForEntered(1));

        std::jthread waiter(
            [&] { waiting = hipObjGet(nullptr, buffer(1), kBufSize, 0, &plainOps_, nullptr).opError; });
        std::this_thread::sleep_for(kStillWaiting);
        EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
        waiter.join();
        EXPECT_EQ(waiting, hipObjNotInitialized);
        gate.release(1);
    }
    EXPECT_EQ(holding, hipObjSuccess);
}

// ---- Buffers -----------------------------------------------------------

TEST_F(ThreadSafetyTest, DeregisterIsBusyWhileATransferUsesTheBuffer)
{
    ASSERT_EQ(init(), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(1));

    TransferGate    gate;
    hipObjOpError_t result = hipObjInternalError;
    {
        const std::jthread transfer(
            [&] { result = hipObjPut(nullptr, buffer(0), kBufSize, 0, &gateOps_, &gate).opError; });
        EXPECT_TRUE(gate.waitForEntered(1));
        EXPECT_EQ(hipObjBufDeregister(buffer(0)).opError, hipObjBusy);
        gate.release(1);
    }
    EXPECT_EQ(result, hipObjSuccess);
    EXPECT_EQ(hipDevice_.log.deregMr, 0);
    EXPECT_EQ(hipObjBufDeregister(buffer(0)).opError, hipObjSuccess);
}

/* Of two threads that register the same buffer at once, one succeeds, and
 * the other's registration, if it made one, is released */
TEST_F(ThreadSafetyTest, RegisteringTheSameBufferHasOneWinner)
{
    ASSERT_EQ(init(), hipObjSuccess);

    for (int i = 0; i < kIterations; ++i) {
        std::atomic<int> registered{0};
        std::atomic<int> alreadyRegistered{0};
        runThreads(2, [&](int) {
            const hipObjOpError_t err = hipObjBufRegisterHost(buffer(0), kBufSize).opError;
            if (err == hipObjSuccess) {
                registered.fetch_add(1);
            }
            else if (err == hipObjBufAlreadyRegistered) {
                alreadyRegistered.fetch_add(1);
            }
            else {
                ADD_FAILURE() << "registration returned " << err;
            }
        });
        EXPECT_EQ(registered.load(), 1);
        EXPECT_EQ(alreadyRegistered.load(), 1);
        EXPECT_EQ(hipDevice_.log.registerMr.load() - hipDevice_.log.deregMr.load(), 1);
        EXPECT_EQ(hipObjBufDeregister(buffer(0)).opError, hipObjSuccess);
    }
}

TEST_F(ThreadSafetyTest, BufferCallsFromManyThreads)
{
    ASSERT_EQ(init(), hipObjSuccess);

    /* Each thread works on its own buffer, so every call succeeds, while
     * the other threads' calls change the buffer map under it */
    std::atomic<int> transfers{0};
    runThreads(kThreads, [&](int t) {
        void *buf = buffer(t);
        for (int i = 0; i < kIterations; ++i) {
            EXPECT_EQ(hipObjBufRegisterHost(buf, kBufSize).opError, hipObjSuccess);
            EXPECT_EQ(hipObjGet(nullptr, buf, kBufSize, 0, &countingOps_, &transfers).opError, hipObjSuccess);
            EXPECT_EQ(hipObjBufSync(buf, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
            EXPECT_EQ(hipObjPut(nullptr, buf, kBufSize, 0, &countingOps_, &transfers).opError, hipObjSuccess);
            EXPECT_EQ(hipObjBufSync(buf, kBufSize, 0, HIPOBJ_SYNC_TO_DEVICE).opError, hipObjSuccess);

            char *token = nullptr;
            EXPECT_EQ(hipObjGetRdmaToken(buf, kBufSize, HIPOBJ_RDMA_OP_GET, &token).opError, hipObjSuccess);
            if (token) {
                EXPECT_EQ(hipObjPutRdmaToken(token).opError, hipObjSuccess);
            }

            EXPECT_EQ(hipObjBufDeregister(buf).opError, hipObjSuccess);
        }
    });

    EXPECT_EQ(transfers.load(), 2 * kThreads * kIterations);
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(hipObjBufDeregister(buffer(t)).opError, hipObjBufNotRegistered);
    }
}

// ---- Initialization and shutdown ---------------------------------------

TEST_F(ThreadSafetyTest, ConcurrentInitHasOneWinner)
{
    std::atomic<int> succeeded{0};
    std::atomic<int> alreadyInitialized{0};

    runThreads(kThreads, [&](int) {
        const hipObjOpError_t err = init();
        if (err == hipObjSuccess) {
            succeeded.fetch_add(1);
        }
        else if (err == hipObjAlreadyInitialized) {
            alreadyInitialized.fetch_add(1);
        }
        else {
            ADD_FAILURE() << "initialization returned " << err;
        }
    });

    EXPECT_EQ(succeeded.load(), 1);
    EXPECT_EQ(alreadyInitialized.load(), kThreads - 1);
    EXPECT_EQ(hipDevice_.log.openDevice, 1);
}

/* Shutdown doesn't wait for a transfer, or release what it uses. New
 * calls find the library uninitialized, and it can be initialized again,
 * while the transfer finishes with the old instance. */
TEST_F(ThreadSafetyTest, ShutdownLeavesATransferToFinish)
{
    ASSERT_EQ(init(), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(1));
    const hipObjTest::CallLog &log = hipDevice_.log;
    const int                  qps = log.createQp;

    TransferGate    gate;
    hipObjOpError_t result = hipObjInternalError;
    {
        const std::jthread transfer(
            [&] { result = hipObjGet(nullptr, buffer(0), kBufSize, 0, &gateOps_, &gate).opError; });
        EXPECT_TRUE(gate.waitForEntered(1));

        EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
        EXPECT_EQ(log.deregMr, 0);
        EXPECT_EQ(log.destroyQp, 0);
        EXPECT_EQ(log.closeDevice, 0);

        EXPECT_EQ(hipObjBufRegisterHost(buffer(1), kBufSize).opError, hipObjNotInitialized);
        EXPECT_EQ(init(), hipObjSuccess);
        EXPECT_EQ(log.openDevice, 2);
        gate.release(1);
    }
    EXPECT_EQ(result, hipObjSuccess);

    /* The transfer held the old instance last, so it's released now */
    EXPECT_EQ(log.deregMr, 1);
    EXPECT_EQ(log.destroyQp, qps);
    EXPECT_EQ(log.closeDevice, 1);
}

TEST_F(ThreadSafetyTest, ShutdownAndInitDuringBufferCalls)
{
    ASSERT_EQ(init(), hipObjSuccess);

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
                EXPECT_EQ(init(), hipObjSuccess);
            }
            return;
        }
        void *buf = buffer(t);
        for (int i = 0; i < kIterations; ++i) {
            EXPECT_PRED1(possible, hipObjBufRegisterHost(buf, kBufSize).opError);
            EXPECT_PRED1(possible, hipObjGet(nullptr, buf, kBufSize, 0, &plainOps_, nullptr).opError);
            EXPECT_PRED1(possible, hipObjBufDeregister(buf).opError);
        }
    });

    EXPECT_NE(hipObj::currentInstance(), nullptr);
}

// ---- Callbacks ---------------------------------------------------------

/* One call that reentrantSendRequest() made: what it returned, and what
 * it should have */
struct ReentrantCall {
    std::string_view name;
    hipObjOpError_t  result;
    hipObjOpError_t  expected;
};

/* The number of public functions. reentrantSendRequest() calls each one. */
constexpr size_t kPublicFunctions = 15;

/* What reentrantSendRequest() needs, and what it found */
struct ReentryProbe {
    void       *registered = nullptr;
    void       *device     = nullptr;
    void       *host       = nullptr;
    hipObjOps_t plainOps   = {.sendRequest = &plainSendRequest, .recvReply = &plainRecvReply};

    std::vector<ReentrantCall> calls{};
    const char                *errorString = nullptr;
    const char                *version     = nullptr;

    void record(std::string_view name, hipObjError_t result, hipObjOpError_t expected)
    {
        calls.push_back({.name = name, .result = result.opError, .expected = expected});
    }
};

/* Calls every public function from inside a transfer's callback, with
 * arguments that would make each one succeed or change something if it
 * ran on its own. registered is the transfer's buffer, and device and host
 * aren't registered. */
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

TEST_F(ThreadSafetyTest, CallbacksCanCallEveryFunction)
{
    ASSERT_EQ(init(), hipObjSuccess);

    ReentryProbe probe;
    probe.registered = buffer(0);
    probe.device     = buffer(1);
    probe.host       = buffer(2);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(1));

    hipObjOps_t ops = {.sendRequest = &reentrantSendRequest, .recvReply = &plainRecvReply};
    EXPECT_EQ(hipObjGet(nullptr, probe.registered, kBufSize, 0, &ops, &probe).opError, hipObjSuccess);

    /* calls has every function but hipObjGetErrorString() and
     * hipObjGetVersionString(), which return strings rather than an error,
     * and hipObjBufDeregister() a second time, after hipObjShutdown() */
    EXPECT_EQ(probe.calls.size(), kPublicFunctions - 2 + 1);
    for (const ReentrantCall &call : probe.calls) {
        EXPECT_EQ(call.result, call.expected) << call.name << " from a callback";
    }
    EXPECT_STREQ(probe.errorString, "Busy");
    EXPECT_STREQ(probe.version, hipObjGetVersionString());

    /* The callback shut the library down, and the transfer, which held
     * the instance last, released it and every registration in it */
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    EXPECT_EQ(hipDevice_.log.registerMr, 3);
    EXPECT_EQ(hipDevice_.log.deregMr, 3);
    EXPECT_EQ(hipDevice_.log.closeDevice, 1);
}

TEST_F(ThreadSafetyTest, StatelessFunctionsFromManyThreads)
{
    ASSERT_EQ(init(), hipObjSuccess);
    ASSERT_NO_FATAL_FAILURE(registerBuffers(1));
    char *token = nullptr;
    ASSERT_EQ(hipObjGetRdmaToken(buffer(0), kBufSize, HIPOBJ_RDMA_OP_GET, &token).opError, hipObjSuccess);
    /* Releases the token however the test ends */
    const std::unique_ptr<char, decltype(&hipObjPutRdmaToken)> tokenOwner(token, &hipObjPutRdmaToken);

    const std::string expectedVersion = std::to_string(HIPOBJ_VERSION_MAJOR) + "." +
                                        std::to_string(HIPOBJ_VERSION_MINOR) + "." +
                                        std::to_string(HIPOBJ_VERSION_PATCH);
    std::array<const char *, kThreads> versions{};

    runThreads(kThreads, [&](int t) {
        versions.at(static_cast<size_t>(t)) = hipObjGetVersionString();
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
