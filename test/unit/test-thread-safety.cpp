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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <latch>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
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

// ---- Fake libibverbs and HIP -------------------------------------------

std::atomic<int> g_openDeviceCalls{0};

struct ibv_device **
fakeGetDeviceList(int *numDevices)
{
    static std::array<struct ibv_device *, 2> devices = {reinterpret_cast<struct ibv_device *>(0x1), nullptr};
    if (numDevices) {
        *numDevices = 1;
    }
    return devices.data();
}

void
fakeFreeDeviceList(struct ibv_device **)
{
}

const char *
fakeGetDeviceName(struct ibv_device *)
{
    return "mlx5_0";
}

struct ibv_context *
fakeOpenDevice(struct ibv_device *)
{
    g_openDeviceCalls.fetch_add(1);
    return reinterpret_cast<struct ibv_context *>(0x2);
}

int
fakeCloseDevice(struct ibv_context *)
{
    return 0;
}

int
fakeQueryPort(struct ibv_context *, uint8_t, struct ibv_port_attr *attr)
{
    if (attr) {
        std::memset(attr, 0, sizeof(*attr));
    }
    return 0;
}

int
fakeQueryGid(struct ibv_context *, uint8_t, int, union ibv_gid *gid)
{
    if (gid) {
        std::memset(gid, 0, sizeof(*gid));
    }
    return 0;
}

struct ibv_pd *
fakeAllocPd(struct ibv_context *)
{
    return reinterpret_cast<struct ibv_pd *>(0x3);
}

int
fakeDeallocPd(struct ibv_pd *)
{
    return 0;
}

/* The library owns the MR until it passes it to fakeDeregMr() */
struct ibv_mr *
fakeRegMr(struct ibv_pd *, void *addr, size_t, int)
{
    auto mr  = std::make_unique<struct ibv_mr>();
    mr->addr = addr;
    mr->rkey = 0x1234;
    return mr.release();
}

struct ibv_mr *
fakeRegMrIova2(struct ibv_pd *pd, void *addr, size_t size, uintptr_t, int access)
{
    return fakeRegMr(pd, addr, size, access);
}

int
fakeDeregMr(struct ibv_mr *mr)
{
    std::unique_ptr<struct ibv_mr> owned(mr);
    return 0;
}

struct ibv_cq *
fakeCreateCq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int)
{
    return reinterpret_cast<struct ibv_cq *>(0x4);
}

int
fakeDestroyCq(struct ibv_cq *)
{
    return 0;
}

/* The library owns the QP until it passes it to fakeDestroyQp() */
struct ibv_qp *
fakeCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
{
    auto qp    = std::make_unique<struct ibv_qp>();
    qp->qp_num = 0x55;
    return qp.release();
}

int
fakeDestroyQp(struct ibv_qp *qp)
{
    std::unique_ptr<struct ibv_qp> owned(qp);
    return 0;
}

int
fakeModifyQp(struct ibv_qp *, struct ibv_qp_attr *, int)
{
    return 0;
}

int
fakePollCq(struct ibv_cq *, int, struct ibv_wc *wc)
{
    if (wc) {
        std::memset(wc, 0, sizeof(*wc));
        wc->status = IBV_WC_SUCCESS;
    }
    return 1;
}

/* With no GPU and a NIC hint, hipObjInit() opens the hinted NIC directly */
hipError_t
fakeGetDeviceNoDevice(int *)
{
    return hipErrorNoDevice;
}

// ---- Transfer callbacks ------------------------------------------------

int
plainSendRequest(void *, const char *, size_t)
{
    return 0;
}

int
plainRecvReply(void *, char *reply, size_t *replyLen)
{
    static constexpr std::string_view kReply = "200";
    if (!reply || !replyLen || *replyLen < kReply.size()) {
        return -1;
    }
    std::memcpy(reply, kReply.data(), kReply.size());
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
probeSendRequest(void *ctx, const char *, size_t)
{
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
    hipObjConfig_t config = {};
    config.gpuDevice      = -1;
    config.nicHint        = "mlx5_0";
    return config;
}

/* Installs the fakes and a fresh driver state for each test, and puts
 * the real ones back afterward */
class ThreadSafetyTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        savedState_ = hipObj::setStateForTest(&state_);

        savedHipOps_       = hipObj::hipOps();
        hipObj::HipOps ops = savedHipOps_;
        ops.hipGetDevice   = &fakeGetDeviceNoDevice;
        /* Without it, every pointer counts as host memory, so registration
         * never calls into the HIP runtime */
        ops.hipPointerGetAttributes = nullptr;
        hipObj::hipOps()            = ops;

        auto &funcs            = hipObj::ibv.funcsForTest();
        savedFuncs_            = funcs;
        funcs.get_device_list  = &fakeGetDeviceList;
        funcs.free_device_list = &fakeFreeDeviceList;
        funcs.get_device_name  = &fakeGetDeviceName;
        funcs.open_device      = &fakeOpenDevice;
        funcs.close_device     = &fakeCloseDevice;
        funcs.query_port       = &fakeQueryPort;
        funcs.query_gid        = &fakeQueryGid;
        funcs.alloc_pd         = &fakeAllocPd;
        funcs.dealloc_pd       = &fakeDeallocPd;
        funcs.reg_mr           = &fakeRegMr;
        funcs.reg_mr_iova2     = &fakeRegMrIova2;
        funcs.dereg_mr         = &fakeDeregMr;
        funcs.create_cq        = &fakeCreateCq;
        funcs.destroy_cq       = &fakeDestroyCq;
        funcs.create_qp        = &fakeCreateQp;
        funcs.destroy_qp       = &fakeDestroyQp;
        funcs.modify_qp        = &fakeModifyQp;
        funcs.poll_cq          = &fakePollCq;

        savedIbvInitialized_       = hipObj::ibv.is_initialized;
        hipObj::ibv.is_initialized = true;
        g_openDeviceCalls          = 0;

        probeOps_.sendRequest = &probeSendRequest;
        probeOps_.recvReply   = &probeRecvReply;
    }

    void TearDown() override
    {
        (void)hipObjShutdown();
        hipObj::ibv.is_initialized = savedIbvInitialized_;
        hipObj::ibv.funcsForTest() = savedFuncs_;
        hipObj::hipOps()           = savedHipOps_;
        hipObj::setStateForTest(savedState_);
    }

    /* The buffer for thread index, kBufSize bytes, distinct from every
     * other thread's */
    void *buffer(int index)
    {
        return pool_.data() + static_cast<size_t>(index) * kBufSize;
    }

    hipObj::DriverState  state_;
    hipObj::DriverState *savedState_ = nullptr;
    hipObj::IbvFuncs     savedFuncs_ = {};
    hipObj::HipOps       savedHipOps_;
    bool                 savedIbvInitialized_ = false;
    std::vector<char>    pool_                = std::vector<char>(kThreads * kBufSize);
    TransferProbe        probe_;
    hipObjOps_t          probeOps_ = {};
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
    EXPECT_EQ(g_openDeviceCalls.load(), 1);
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
    EXPECT_TRUE(state_.initialized);
}

#ifdef HIPOBJECT_V2_API
/* hipObjGetV2() and hipObjPutV2() check that these are set, but don't
 * call them yet */
int
unusedSendPrepare(void *, const hipObjTransferReqV2_t *, hipObjPrepareReplyV2_t *)
{
    return -1;
}

int
unusedSendReady(void *, const hipObjTransferReqV2_t *, hipObjFinalReplyV2_t *)
{
    return -1;
}

int
unusedSendCancel(void *, const hipObjTransferReqV2_t *)
{
    return -1;
}

/* The number of functions reentrantSendRequest() calls that take the
 * API lock */
constexpr size_t kLockedFunctions = 11;
#else
constexpr size_t kLockedFunctions = 9;
#endif

/* What reentrantSendRequest() needs, and what it found */
struct ReentryProbe {
    void       *registered   = nullptr;
    void       *unregistered = nullptr;
    hipObjOps_t plainOps     = {&plainSendRequest, &plainRecvReply};
#ifdef HIPOBJECT_V2_API
    hipObjOpsV2_t v2Ops = {{}, &unusedSendPrepare, &unusedSendReady, &unusedSendCancel};
#endif

    std::vector<std::pair<std::string_view, hipObjOpError_t>> locked;
    char                                                     *token       = nullptr;
    hipObjOpError_t                                           parseResult = hipObjInternalError;
    int                                                       parsedCode  = 0;
};

/* Calls every function that takes the API lock, with arguments that would
 * make each one succeed or change something if it ran, and then one that
 * doesn't take the lock */
int
reentrantSendRequest(void *ctx, const char *, size_t)
{
    auto          *probe  = static_cast<ReentryProbe *>(ctx);
    hipObjConfig_t config = makeConfig();

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
#ifdef HIPOBJECT_V2_API
    void          *buf   = probe->registered;
    hipObjOpsV2_t *v2Ops = &probe->v2Ops;
    probe->locked.emplace_back("hipObjGetV2",
                               hipObjGetV2("b", "k", buf, kBufSize, 0, nullptr, v2Ops, nullptr).opError);
    probe->locked.emplace_back("hipObjPutV2",
                               hipObjPutV2("b", "k", buf, kBufSize, 0, nullptr, v2Ops, nullptr).opError);
#endif

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
    EXPECT_TRUE(state_.initialized);
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
    std::array<const char *, kThreads> versions = {};

    runThreads(kThreads, [&](int t) {
        versions[static_cast<size_t>(t)] = hipObjGetVersionString();
        for (int i = 0; i < kIterations; ++i) {
            EXPECT_STREQ(hipObjGetErrorString(hipObjBufNotRegistered), "Buffer not registered");

            static constexpr std::string_view kReply = "206";
            int                               code   = 0;
            EXPECT_EQ(hipObjParseRdmaReply(kReply.data(), kReply.size(), &code).opError, hipObjSuccess);
            EXPECT_EQ(code, 206);

            std::array<char, 32> nicIp = {};
            EXPECT_EQ(hipObjTokenClientNic(token, nicIp.data(), nicIp.size()).opError, hipObjSuccess);
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
