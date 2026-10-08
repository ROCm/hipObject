/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "fake-device.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace hipObjTest {

FakeDevice &
fake()
{
    static FakeDevice device;
    return device;
}

void
setReply(std::string_view reply)
{
    FakeDevice &f = fake();
    ASSERT_LT(reply.size(), f.reply.size());
    std::memcpy(f.reply.data(), reply.data(), reply.size());
    f.reply[reply.size()] = '\0';
    f.replyLen            = reply.size() + 1;
}

hipObj::RdmaToken
peerToken(uint8_t transport)
{
    hipObj::RdmaToken token{};
    token.transport = transport;
    token.qpNum     = 0x77;
    for (size_t i = 0; i < sizeof(token.gid); ++i) {
        token.gid[i] = static_cast<uint8_t>(0xf0 + i);
    }
    token.rkey       = 0x4321;
    token.remoteAddr = 0x900000;
    token.length     = kFakeBufSize;
    token.portNum    = 1;
    token.lid        = 0x12;
    return token;
}

void
setReplyWithPeerToken(const hipObj::RdmaToken &token)
{
    setReply(hipObj::encodeReplyWithPeerToken(200, token));
}

std::vector<ModifyQpCall>
modifyQpCalls()
{
    const FakeDevice &f = fake();
    return {f.modifyQp.begin(), f.modifyQp.begin() + static_cast<std::ptrdiff_t>(f.modifyQpCount)};
}

namespace {

    // ---- ibverbs -----------------------------------------------------

    struct ibv_device *g_devices[] = {reinterpret_cast<struct ibv_device *>(0x1), nullptr};

    struct ibv_device **fakeGetDeviceList(int *numDevices)
    {
        if (fake().fail.getDeviceList) {
            return nullptr;
        }
        if (numDevices) {
            *numDevices = 1;
        }
        return g_devices;
    }

    void fakeFreeDeviceList(struct ibv_device **)
    {
    }

    struct ibv_context *fakeOpenDevice(struct ibv_device *)
    {
        if (fake().fail.openDevice) {
            return nullptr;
        }
        ++fake().log.openDevice;
        return reinterpret_cast<struct ibv_context *>(0x2);
    }

    int fakeCloseDevice(struct ibv_context *)
    {
        ++fake().log.closeDevice;
        return 0;
    }

    const char *fakeGetDeviceName(struct ibv_device *)
    {
        return "mlx5_0";
    }

    int fakeQueryDevice(struct ibv_context *, struct ibv_device_attr *attr)
    {
        if (fake().fail.queryDevice) {
            return -1;
        }
        std::memset(attr, 0, sizeof(*attr));
        attr->vendor_id = fake().vendorId;
        return 0;
    }

    /* An empty GID table, so GID selection never looks at the device */
    int fakeQueryPort(struct ibv_context *, uint8_t, struct ibv_port_attr *attr)
    {
        if (fake().fail.queryPort) {
            return -1;
        }
        if (attr) {
            std::memset(attr, 0, sizeof(*attr));
        }
        return 0;
    }

    int fakeQueryGid(struct ibv_context *, uint8_t, int, union ibv_gid *gid)
    {
        if (fake().fail.queryGid) {
            return -1;
        }
        if (gid) {
            std::memset(gid, 0, sizeof(*gid));
        }
        return 0;
    }

    struct ibv_pd *fakeAllocPd(struct ibv_context *)
    {
        if (fake().fail.allocPd) {
            return nullptr;
        }
        ++fake().log.allocPd;
        return reinterpret_cast<struct ibv_pd *>(0x3);
    }

    int fakeDeallocPd(struct ibv_pd *)
    {
        ++fake().log.deallocPd;
        return 0;
    }

    struct ibv_mr *fakeRegisterMr(struct ibv_pd *, void *addr, size_t, int access)
    {
        FakeDevice &f = fake();
        ++f.log.registerMr;
        f.log.mrAccess = access;
        if (f.fail.registerAll || addr == f.fail.registerAddr) {
            return nullptr;
        }
        auto *mr = static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
        if (mr) {
            mr->addr = addr;
            mr->rkey = 0x1234;
        }
        return mr;
    }

    struct ibv_mr *fakeRegisterMrIova2(struct ibv_pd *pd, void *addr, size_t size, uintptr_t, int access)
    {
        return fakeRegisterMr(pd, addr, size, access);
    }

    int fakeDeregisterMr(struct ibv_mr *mr)
    {
        std::free(mr);
        return 0;
    }

    struct ibv_cq *fakeCreateCq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int)
    {
        if (fake().fail.createCq) {
            return nullptr;
        }
        ++fake().log.createCq;
        return reinterpret_cast<struct ibv_cq *>(0x4);
    }

    int fakeDestroyCq(struct ibv_cq *)
    {
        ++fake().log.destroyCq;
        return 0;
    }

    struct ibv_qp *fakeCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
    {
        if (fake().fail.createQp) {
            return nullptr;
        }
        auto *qp = static_cast<struct ibv_qp *>(std::calloc(1, sizeof(struct ibv_qp)));
        if (qp) {
            ++fake().log.createQp;
            qp->qp_num = 0x55;
        }
        return qp;
    }

    int fakeModifyQp(struct ibv_qp *, struct ibv_qp_attr *attr, int mask)
    {
        FakeDevice &f = fake();
        if (f.modifyQpCount < f.modifyQp.size()) {
            f.modifyQp[f.modifyQpCount++] = {*attr, mask};
        }
        return static_cast<int>(attr->qp_state) == f.fail.modifyQpToState ? -1 : 0;
    }

    int fakeDestroyQp(struct ibv_qp *qp)
    {
        ++fake().log.destroyQp;
        std::free(qp);
        return 0;
    }

    int fakePollCq(struct ibv_cq *, int, struct ibv_wc *wc)
    {
        const FakeDevice &f = fake();
        if (f.pollResult <= 0) {
            return f.pollResult;
        }
        std::memset(wc, 0, sizeof(*wc));
        wc->status = f.pollStatus;
        wc->opcode = f.pollOpcode;
        return 1;
    }

    // ---- HIP ---------------------------------------------------------

    hipError_t fakeGetDeviceNoDevice(int *)
    {
        return hipErrorNoDevice;
    }

    hipError_t fakeGetDeviceCount(int *count)
    {
        const FakeDevice &f = fake();
        *count              = f.deviceCountErr == hipSuccess ? f.deviceCount : 0;
        return f.deviceCountErr;
    }

    /* No topology for any GPU, so hipObjInit() opens the hinted NIC */
    hipError_t fakeDeviceGetPCIBusId(char *, int, int)
    {
        return hipErrorInvalidDevice;
    }

    hipError_t fakeDeviceSynchronize()
    {
        return fake().deviceSyncErr;
    }

    hipError_t fakeHostMalloc(void **ptr, size_t size, unsigned int)
    {
        if (fake().fail.hostMalloc) {
            return hipErrorOutOfMemory;
        }
        *ptr = std::malloc(size);
        return *ptr ? hipSuccess : hipErrorOutOfMemory;
    }

    hipError_t fakeHostFree(void *ptr)
    {
        std::free(ptr);
        return hipSuccess;
    }

    hipError_t fakeMemcpy(void *, const void *, size_t, hipMemcpyKind)
    {
        ++fake().log.memcpy;
        return hipSuccess;
    }

} // namespace

hipError_t
fakePointerGetAttributes(hipPointerAttribute_t *attr, const void *)
{
    *attr      = {};
    attr->type = fake().memoryType;
    return hipSuccess;
}

hipError_t
fakeMemGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t)
{
    FakeDevice &f = fake();
    ++f.addressRangeCalls;
    if (f.addressRangeErr != hipSuccess) {
        return f.addressRangeErr;
    }
    *base = reinterpret_cast<hipDeviceptr_t>(f.allocBase);
    *size = f.allocSize;
    return hipSuccess;
}

// ---- S3 callbacks ----------------------------------------------------

int
fakeSendRequest(void *, const char *, size_t)
{
    ++fake().log.sendRequest;
    return 0;
}

int
fakeRecvReply(void *, char *reply, size_t *replyLen)
{
    FakeDevice &f = fake();
    ++f.log.recvReply;
    if (!replyLen || *replyLen < f.replyLen) {
        return -1;
    }
    std::memcpy(reply, f.reply.data(), f.replyLen);
    *replyLen = f.replyLen;
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

std::vector<hipObj::NicInfo>
EmptyNicEnumerator::Enumerate(const char *)
{
    return {};
}

// ---- Fixture ---------------------------------------------------------

void
FakeDeviceTest::SetUp()
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
    funcs.query_device     = &fakeQueryDevice;
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

    fake() = FakeDevice{};
}

void
FakeDeviceTest::TearDown()
{
    (void)hipObjShutdown();
    hipObj::ibv.is_initialized = savedIbvInitialized_;
    hipObj::ibv.funcsForTest() = savedFuncs_;
    hipObj::hipOps()           = savedHipOps_;
    hipObj::setNicEnumerator(savedEnumerator_);
    hipObj::setStateForTest(savedState_);
}

hipObjConfig_t
FakeDeviceTest::makeConfig()
{
    hipObjConfig_t config = {};
    config.gpuDevice      = -1;
    config.nicHint        = "mlx5_0";
    return config;
}

void
FakeDeviceTest::init()
{
    hipObjConfig_t config = makeConfig();
    ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);
}

void
FakeDeviceTest::initAndRegister()
{
    ASSERT_NO_FATAL_FAILURE(init());
    ASSERT_EQ(hipObjBufRegister(fakeDevBuf(), kFakeBufSize).opError, hipObjSuccess);
}

} // namespace hipObjTest
