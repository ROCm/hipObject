/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Argument checks in the public API. Every function in hipobj.h is called
 * with each kind of bad argument, and with arguments just inside each
 * limit, without GPU or RDMA hardware: the fixture substitutes fakes for
 * libibverbs, the HIP runtime, NIC enumeration, and the driver state.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sys/types.h>

#include "hip-seam.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "state.h"
#include "token.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

// ---- Fakes -------------------------------------------------------

struct CallLog {
    int sendRequest = 0;
    int recvReply   = 0;
    int registerMr  = 0;
    int memcpy      = 0;
};

CallLog g_log;

/* reg_mr fails for this address, so hipObjBufRegister() falls back to a
 * host staging buffer */
void *g_failRegisterAddr = nullptr;

int        g_deviceCount    = 1;
hipError_t g_deviceCountErr = hipSuccess;

struct ibv_device *g_devices[] = {reinterpret_cast<struct ibv_device *>(0x1), nullptr};

struct ibv_device **
fakeGetDeviceList(int *numDevices)
{
    if (numDevices) {
        *numDevices = 1;
    }
    return g_devices;
}

void
fakeFreeDeviceList(struct ibv_device **)
{
}

struct ibv_context *
fakeOpenDevice(struct ibv_device *)
{
    return reinterpret_cast<struct ibv_context *>(0x2);
}

int
fakeCloseDevice(struct ibv_context *)
{
    return 0;
}

const char *
fakeGetDeviceName(struct ibv_device *)
{
    return "mlx5_0";
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

struct ibv_mr *
fakeRegisterMr(struct ibv_pd *, void *addr, size_t, int)
{
    ++g_log.registerMr;
    if (addr == g_failRegisterAddr) {
        return nullptr;
    }
    auto *mr = static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
    if (mr) {
        mr->addr = addr;
        mr->rkey = 0x1234;
    }
    return mr;
}

struct ibv_mr *
fakeRegisterMrIova2(struct ibv_pd *pd, void *addr, size_t size, uintptr_t, int access)
{
    return fakeRegisterMr(pd, addr, size, access);
}

int
fakeDeregisterMr(struct ibv_mr *mr)
{
    std::free(mr);
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

struct ibv_qp *
fakeCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
{
    auto *qp = static_cast<struct ibv_qp *>(std::calloc(1, sizeof(struct ibv_qp)));
    if (qp) {
        qp->qp_num = 0x55;
    }
    return qp;
}

int
fakeModifyQp(struct ibv_qp *, struct ibv_qp_attr *, int)
{
    return 0;
}

int
fakeDestroyQp(struct ibv_qp *qp)
{
    std::free(qp);
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

hipError_t
fakeGetDeviceNoDevice(int *)
{
    return hipErrorNoDevice;
}

hipError_t
fakeGetDeviceCount(int *count)
{
    *count = g_deviceCountErr == hipSuccess ? g_deviceCount : 0;
    return g_deviceCountErr;
}

/* No topology for any GPU, so hipObjInit() opens the hinted NIC */
hipError_t
fakeDeviceGetPCIBusId(char *, int, int)
{
    return hipErrorInvalidDevice;
}

hipError_t
fakeDeviceSynchronize()
{
    return hipSuccess;
}

hipError_t
fakeHostMalloc(void **ptr, size_t size, unsigned int)
{
    *ptr = std::malloc(size);
    return *ptr ? hipSuccess : hipErrorOutOfMemory;
}

hipError_t
fakeHostFree(void *ptr)
{
    std::free(ptr);
    return hipSuccess;
}

hipError_t
fakeMemcpy(void *, const void *, size_t, hipMemcpyKind)
{
    ++g_log.memcpy;
    return hipSuccess;
}

class EmptyNicEnumerator : public hipObj::NicEnumerator {
public:
    std::vector<hipObj::NicInfo> Enumerate(const char *) override
    {
        return {};
    }
};

int
fakeSendRequest(void *, const char *, size_t)
{
    ++g_log.sendRequest;
    return 0;
}

int
fakeRecvReply(void *, char *reply, size_t *replyLen)
{
    static constexpr char kReply[] = "200";
    ++g_log.recvReply;
    if (!replyLen || *replyLen < sizeof(kReply)) {
        return -1;
    }
    std::memcpy(reply, kReply, sizeof(kReply));
    *replyLen = sizeof(kReply);
    return 0;
}

hipObjOps_t
makeOps()
{
    hipObjOps_t ops = {};
    ops.sendRequest = &fakeSendRequest;
    ops.recvReply   = &fakeRecvReply;
    return ops;
}

/* Fake device addresses. reg_mr is faked, so nothing reads or writes
 * them. */
void *const kDevBuf = reinterpret_cast<void *>(0x10000);

constexpr size_t kBufSize = 64;
constexpr size_t kMaxMr   = 4ULL * 1024 * 1024 * 1024;

// ---- Fixture -----------------------------------------------------

class ApiArgsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        savedState_      = hipObj::setStateForTest(&state_);
        savedEnumerator_ = hipObj::setNicEnumerator(&enumerator_);

        savedHipOps_                = hipObj::hipOps();
        hipObj::HipOps ops          = savedHipOps_;
        ops.hipGetDevice            = &fakeGetDeviceNoDevice;
        ops.hipGetDeviceCount       = &fakeGetDeviceCount;
        ops.hipDeviceGetPCIBusId    = &fakeDeviceGetPCIBusId;
        ops.hipDeviceSynchronize    = &fakeDeviceSynchronize;
        ops.hipHostMalloc           = &fakeHostMalloc;
        ops.hipHostFree             = &fakeHostFree;
        ops.hipPointerGetAttributes = nullptr;
        /* Without the async entries, staging uses hipMemcpy */
        ops.hipMemcpy       = &fakeMemcpy;
        ops.hipMemcpyAsync  = nullptr;
        ops.hipEventCreate  = nullptr;
        ops.hipEventRecord  = nullptr;
        ops.hipEventQuery   = nullptr;
        ops.hipEventDestroy = nullptr;
        hipObj::hipOps()    = ops;

        auto &funcs            = hipObj::ibv.funcsForTest();
        savedFuncs_            = funcs;
        funcs.get_device_list  = &fakeGetDeviceList;
        funcs.free_device_list = &fakeFreeDeviceList;
        funcs.open_device      = &fakeOpenDevice;
        funcs.close_device     = &fakeCloseDevice;
        funcs.get_device_name  = &fakeGetDeviceName;
        funcs.query_port       = &fakeQueryPort;
        funcs.query_gid        = &fakeQueryGid;
        funcs.alloc_pd         = &fakeAllocPd;
        funcs.dealloc_pd       = &fakeDeallocPd;
        funcs.reg_mr           = &fakeRegisterMr;
        funcs.reg_mr_iova2     = &fakeRegisterMrIova2;
        funcs.dereg_mr         = &fakeDeregisterMr;
        funcs.create_cq        = &fakeCreateCq;
        funcs.destroy_cq       = &fakeDestroyCq;
        funcs.create_qp        = &fakeCreateQp;
        funcs.modify_qp        = &fakeModifyQp;
        funcs.destroy_qp       = &fakeDestroyQp;
        funcs.poll_cq          = &fakePollCq;

        savedIbvInitialized_       = hipObj::ibv.is_initialized;
        hipObj::ibv.is_initialized = true;

        g_log              = {};
        g_failRegisterAddr = nullptr;
        g_deviceCount      = 1;
        g_deviceCountErr   = hipSuccess;
    }

    void TearDown() override
    {
        (void)hipObjShutdown();
        hipObj::ibv.is_initialized = savedIbvInitialized_;
        hipObj::ibv.funcsForTest() = savedFuncs_;
        hipObj::hipOps()           = savedHipOps_;
        hipObj::setNicEnumerator(savedEnumerator_);
        hipObj::setStateForTest(savedState_);
    }

    static hipObjConfig_t makeConfig()
    {
        hipObjConfig_t config = {};
        config.gpuDevice      = -1;
        config.nicHint        = "mlx5_0";
        return config;
    }

    static void init()
    {
        hipObjConfig_t config = makeConfig();
        ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);
    }

    /* Initializes and registers kBufSize bytes at kDevBuf */
    static void initAndRegister()
    {
        ASSERT_NO_FATAL_FAILURE(init());
        ASSERT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjSuccess);
    }

    hipObj::DriverState    state_;
    hipObj::DriverState   *savedState_ = nullptr;
    EmptyNicEnumerator     enumerator_;
    hipObj::NicEnumerator *savedEnumerator_ = nullptr;
    hipObj::IbvFuncs       savedFuncs_      = {};
    hipObj::HipOps         savedHipOps_;
    bool                   savedIbvInitialized_ = false;
};

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
    EXPECT_EQ(g_log.sendRequest, 0);

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
    g_deviceCount = 2;
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
    g_deviceCountErr      = hipErrorNoDevice;
    hipObjConfig_t config = makeConfig();
    config.gpuDevice      = 0;
    EXPECT_EQ(hipObjInit(&config).opError, hipObjInvalidValue);
    EXPECT_FALSE(state_.initialized);
}

TEST_F(ApiArgsTest, InitReportsDeviceCountFailure)
{
    g_deviceCountErr      = hipErrorInvalidValue;
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
    EXPECT_EQ(g_log.registerMr, 0);
}

TEST_P(RegisterArgsTest, RejectsZeroSize)
{
    ASSERT_NO_FATAL_FAILURE(init());
    EXPECT_EQ(GetParam()(kDevBuf, 0).opError, hipObjInvalidValue);
    EXPECT_EQ(g_log.registerMr, 0);
    EXPECT_EQ(hipObjBufDeregister(kDevBuf).opError, hipObjBufNotRegistered);
}

TEST_P(RegisterArgsTest, RejectsMoreThanMaxSize)
{
    ASSERT_NO_FATAL_FAILURE(init());
    for (size_t size : {kMaxMr + 1, std::numeric_limits<size_t>::max()}) {
        EXPECT_EQ(GetParam()(kDevBuf, size).opError, hipObjSizeTooLarge) << "size " << size;
    }
    EXPECT_EQ(g_log.registerMr, 0);

    EXPECT_EQ(GetParam()(kDevBuf, kMaxMr).opError, hipObjSuccess);
}

TEST_P(RegisterArgsTest, RejectsBufferThatWrapsAddressSpace)
{
    ASSERT_NO_FATAL_FAILURE(init());
    void *top = reinterpret_cast<void *>(std::numeric_limits<uintptr_t>::max() - 15);
    EXPECT_EQ(GetParam()(top, 16).opError, hipObjInvalidValue);
    EXPECT_EQ(GetParam()(top, kMaxMr).opError, hipObjInvalidValue);
    EXPECT_EQ(g_log.registerMr, 0);

    EXPECT_EQ(GetParam()(top, 15).opError, hipObjSuccess);
}

TEST_P(RegisterArgsTest, RejectsSecondRegistration)
{
    ASSERT_NO_FATAL_FAILURE(init());
    ASSERT_EQ(GetParam()(kDevBuf, kBufSize).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufRegister(kDevBuf, kBufSize).opError, hipObjBufAlreadyRegistered);
    EXPECT_EQ(hipObjBufRegisterHost(kDevBuf, kBufSize).opError, hipObjBufAlreadyRegistered);
    EXPECT_EQ(g_log.registerMr, 1);
}

INSTANTIATE_TEST_SUITE_P(ApiArgs, RegisterArgsTest,
                         ::testing::Values(&hipObjBufRegister, &hipObjBufRegisterHost),
                         [](const ::testing::TestParamInfo<RegisterFn> &paramInfo) {
                             return paramInfo.param == &hipObjBufRegister ? "Device" : "Host";
                         });

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
        EXPECT_EQ(g_log.sendRequest, 0);
        EXPECT_EQ(g_log.recvReply, 0);
        EXPECT_EQ(g_log.memcpy, 0);
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
    EXPECT_EQ(g_log.sendRequest, 2);
    EXPECT_EQ(g_log.recvReply, 2);
}

TEST_P(TransferArgsTest, ChecksStagedBuffersBeforeCopying)
{
    ASSERT_NO_FATAL_FAILURE(init());
    void *staged       = reinterpret_cast<void *>(0x30000);
    g_failRegisterAddr = staged;
    ASSERT_EQ(hipObjBufRegister(staged, kBufSize).opError, hipObjSuccess);

    hipObjOps_t ops = makeOps();
    expectRejected(staged, 2, kBufSize - 1, &ops, hipObjInvalidValue);
    expectRejected(staged, std::numeric_limits<size_t>::max(), 1, &ops, hipObjInvalidValue);

    EXPECT_EQ(GetParam()(staged, 1, kBufSize - 1, &ops).opError, hipObjSuccess);
    EXPECT_EQ(g_log.memcpy, 1);
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
    EXPECT_EQ(g_log.memcpy, 0);
}

TEST_F(ApiArgsTest, SyncRejectsRangeOutsideStagedBuffer)
{
    ASSERT_NO_FATAL_FAILURE(init());
    void *staged       = reinterpret_cast<void *>(0x30000);
    g_failRegisterAddr = staged;
    ASSERT_EQ(hipObjBufRegister(staged, kBufSize).opError, hipObjSuccess);

    for (int direction : {HIPOBJ_SYNC_TO_HOST, HIPOBJ_SYNC_TO_DEVICE}) {
        EXPECT_EQ(hipObjBufSync(staged, kBufSize + 1, 0, direction).opError, hipObjInvalidValue);
        EXPECT_EQ(hipObjBufSync(staged, std::numeric_limits<size_t>::max(), 1, direction).opError,
                  hipObjInvalidValue);
    }
    EXPECT_EQ(g_log.memcpy, 0);

    EXPECT_EQ(hipObjBufSync(staged, kBufSize, 0, HIPOBJ_SYNC_TO_HOST).opError, hipObjSuccess);
    EXPECT_EQ(hipObjBufSync(staged, 1, kBufSize - 1, HIPOBJ_SYNC_TO_DEVICE).opError, hipObjSuccess);
    EXPECT_EQ(g_log.memcpy, 2);
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
