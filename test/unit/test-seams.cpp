/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the test seams (HIP runtime, NIC topology, driver
 * state, libibverbs). All run without GPU or RDMA hardware by
 * substituting fakes through the seam accessors.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>
#include <hsa/hsa_ext_amd.h>

#include "fake-device.h"
#include "fake-ibv.h"
#include "hip-seam.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "rdma-topology.h"
#include "state.h"
#include "transport.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

// ---- HIP seam ----------------------------------------------------

/* The GPU's bus ID in the NIC topology tests */
constexpr std::string_view kGpuBusId = "0000:42:00.0";

TEST(HipSeamTest, OverrideRedirectsHip)
{
    hipObjTest::FakeDevice    device{};
    const hipObjTest::FakeHip fake(device);

    const hipObj::HipRuntime *previous = hipObj::setHipForTest(&fake);
    EXPECT_EQ(previous, nullptr);
    EXPECT_EQ(&hipObj::hip(), &fake);

    device.getDeviceErr = hipSuccess;
    device.device       = 7;
    int current         = -1;
    EXPECT_EQ(hipObj::hip().getDevice(&current), hipSuccess);
    EXPECT_EQ(current, 7);

    device.gpuBusId = kGpuBusId;
    std::array<char, 32> busId{};
    EXPECT_EQ(hipObj::hip().deviceGetPCIBusId(busId.data(), static_cast<int>(busId.size()), 0), hipSuccess);
    EXPECT_EQ(std::string_view(busId.data()), kGpuBusId);

    EXPECT_EQ(hipObj::setHipForTest(previous), &fake);
    EXPECT_NE(&hipObj::hip(), &fake);
}

/* SystemHip passes each call straight to HIP or HSA, so it gets the same
 * answer as calling them directly, whether or not there's a GPU */
TEST(HipSeamTest, SystemHipForwardsEachCall)
{
    const hipObj::SystemHip system;

    int viaSystem = -1;
    int direct    = -1;
    EXPECT_EQ(system.getDeviceCount(&viaSystem), hipGetDeviceCount(&direct));
    EXPECT_EQ(viaSystem, direct);
    viaSystem = -1;
    direct    = -1;
    EXPECT_EQ(system.getDevice(&viaSystem), hipGetDevice(&direct));
    EXPECT_EQ(viaSystem, direct);

    std::array<char, 32> busIdViaSystem{};
    std::array<char, 32> busIdDirect{};
    EXPECT_EQ(system.deviceGetPCIBusId(busIdViaSystem.data(), static_cast<int>(busIdViaSystem.size()), 0),
              hipDeviceGetPCIBusId(busIdDirect.data(), static_cast<int>(busIdDirect.size()), 0));
    EXPECT_EQ(busIdViaSystem, busIdDirect);

    void *hostViaSystem = nullptr;
    void *hostDirect    = nullptr;
    EXPECT_EQ(system.hostMalloc(&hostViaSystem, 64, hipHostMallocDefault),
              hipHostMalloc(&hostDirect, 64, hipHostMallocDefault));
    EXPECT_EQ(hostViaSystem == nullptr, hostDirect == nullptr);
    EXPECT_EQ(system.hostFree(hostViaSystem), hipHostFree(hostDirect));

    /* A host-to-host copy, which the device synchronization below waits
     * for, so it's done before the arrays are destroyed */
    std::array<char, 8> source = {{'h', 'i', 'p', 'O', 'b', 'j', '\0', '\0'}};
    std::array<char, 8> copyViaSystem{};
    std::array<char, 8> copyDirect{};
    EXPECT_EQ(
        system.memcpyAsync(copyViaSystem.data(), source.data(), source.size(), hipMemcpyHostToHost, nullptr),
        hipMemcpyAsync(copyDirect.data(), source.data(), source.size(), hipMemcpyHostToHost, nullptr));
    EXPECT_EQ(system.deviceSynchronize(), hipDeviceSynchronize());
    EXPECT_EQ(copyViaSystem, copyDirect);

    hipEvent_t eventViaSystem = nullptr;
    hipEvent_t eventDirect    = nullptr;
    EXPECT_EQ(system.eventCreate(&eventViaSystem), hipEventCreate(&eventDirect));
    EXPECT_EQ(system.eventRecord(eventViaSystem, nullptr), hipEventRecord(eventDirect, nullptr));
    (void)hipEventSynchronize(eventViaSystem);
    (void)hipEventSynchronize(eventDirect);
    EXPECT_EQ(system.eventQuery(eventViaSystem), hipEventQuery(eventDirect));
    EXPECT_EQ(system.eventDestroy(eventViaSystem), hipEventDestroy(eventDirect));

    /* Host memory on the stack */
    hipPointerAttribute_t attributesViaSystem{};
    hipPointerAttribute_t attributesDirect{};
    EXPECT_EQ(system.pointerGetAttributes(&attributesViaSystem, source.data()),
              hipPointerGetAttributes(&attributesDirect, source.data()));
    EXPECT_EQ(attributesViaSystem.type, attributesDirect.type);

    hipDeviceptr_t baseViaSystem = nullptr;
    hipDeviceptr_t baseDirect    = nullptr;
    size_t         sizeViaSystem = 0;
    size_t         sizeDirect    = 0;
    EXPECT_EQ(system.memGetAddressRange(&baseViaSystem, &sizeViaSystem, source.data()),
              hipMemGetAddressRange(&baseDirect, &sizeDirect, source.data()));
    EXPECT_EQ(baseViaSystem, baseDirect);
    EXPECT_EQ(sizeViaSystem, sizeDirect);

    int      fdViaSystem     = -1;
    int      fdDirect        = -1;
    uint64_t offsetViaSystem = 0;
    uint64_t offsetDirect    = 0;
    EXPECT_EQ(system.exportDmabuf(source.data(), source.size(), &fdViaSystem, &offsetViaSystem),
              static_cast<int>(
                  hsa_amd_portable_export_dmabuf(source.data(), source.size(), &fdDirect, &offsetDirect)));
    EXPECT_EQ(system.closeDmabuf(fdViaSystem), static_cast<int>(hsa_amd_portable_close_dmabuf(fdDirect)));
}

// ---- NIC topology seam -------------------------------------------

class FakeNicEnumerator : public hipObj::NicEnumerator {
public:
    std::vector<hipObj::NicInfo> Enumerate(const char *hca_list) override
    {
        last_hca_list_ = hca_list ? hca_list : "";
        return nics_;
    }

    std::vector<hipObj::NicInfo> nics_{};
    std::string                  last_hca_list_{};
};

class NicSeamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        previous_        = hipObj::setNicEnumerator(&fake_);
        device_.gpuBusId = kGpuBusId;
        fakeHip_.emplace(hip_);
    }

    void TearDown() override
    {
        fakeHip_.reset();
        hipObj::setNicEnumerator(previous_);
    }

    FakeNicEnumerator                        fake_{};
    hipObj::NicEnumerator                   *previous_ = nullptr;
    hipObjTest::FakeDevice                   device_{};
    hipObjTest::FakeHip                      hip_{device_};
    std::optional<hipObjTest::ScopedFakeHip> fakeHip_{};
};

TEST_F(NicSeamTest, ClosestBusIdWins)
{
    fake_.nics_ = {
        {"mlx5_0", "0000:18:00.0", 0, 1},
        {"mlx5_1", "0000:43:00.0", 1, 1},
        {"mlx5_2", "0000:11:00.0", 0, 1},
    };

    const auto nic = hipObj::GetClosestNicToGpu(0, nullptr);
    // GPU bus 0000:42:00.0: distance 1 for 43, larger for the others.
    ASSERT_TRUE(nic);
    EXPECT_EQ(nic->index, 1);
    EXPECT_EQ(nic->name, "mlx5_1");
}

TEST_F(NicSeamTest, EmptyEnumerationFails)
{
    EXPECT_FALSE(hipObj::GetClosestNicToGpu(0, nullptr));
}

TEST_F(NicSeamTest, NoKnownBusIdFails)
{
    fake_.nics_ = {
        {"mlx5_0", "", 0, 1},
        {"mlx5_1", "", 1, 1},
    };

    EXPECT_FALSE(hipObj::GetClosestNicToGpu(0, nullptr));
}

TEST_F(NicSeamTest, FirstOfClosestWins)
{
    fake_.nics_ = {
        {"mlx5_0", "", 0, 1},
        {"mlx5_1", "0000:43:00.0", 1, 1},
        {"mlx5_2", "0000:43:00.1", 1, 1},
    };

    const auto nic = hipObj::GetClosestNicToGpu(0, nullptr);
    ASSERT_TRUE(nic);
    EXPECT_EQ(nic->index, 1);
    EXPECT_EQ(nic->name, "mlx5_1");
}

TEST_F(NicSeamTest, HcaFilterIsForwarded)
{
    fake_.nics_ = {{"mlx5_0", "0000:18:00.0", 0, 1}};
    hipObj::GetClosestNicToGpu(0, "mlx5_0,mlx5_1");
    EXPECT_EQ(fake_.last_hca_list_, "mlx5_0,mlx5_1");
}

// ---- Driver state seam -------------------------------------------

class StateSeamTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        hipObj::setStateForTest(savedState_);
    }

    // A fixture member rather than a test-body local, so the override
    // stays valid until TearDown() even if the body exits early.
    hipObj::DriverState  fresh_{};
    hipObj::DriverState *savedState_ = nullptr;
};

TEST_F(StateSeamTest, OverrideRedirectsGetState)
{
    fresh_.initialized = true;
    fresh_.gpuDevice   = 3;
    fresh_.endpoint    = "http://example";

    // Installed here rather than in SetUp() so the return value can be
    // checked.
    savedState_ = hipObj::setStateForTest(&fresh_);
    EXPECT_EQ(savedState_, nullptr);

    hipObj::DriverState &state = hipObj::getState();
    EXPECT_EQ(&state, &fresh_);
    EXPECT_TRUE(state.initialized);
    EXPECT_EQ(state.gpuDevice, 3);
    EXPECT_EQ(state.endpoint, "http://example");

    EXPECT_EQ(hipObj::setStateForTest(savedState_), &fresh_);
}

// ---- IBV wrapper seam --------------------------------------------

TEST(IbvWrapperSeamTest, OverrideRedirectsIbv)
{
    hipObj::IBVWrapper fake(hipObj::IbvFuncs{});
    EXPECT_TRUE(fake.is_initialized());

    hipObj::IBVWrapper *previous = hipObj::setIbvForTest(&fake);
    EXPECT_EQ(previous, nullptr);
    EXPECT_EQ(&hipObj::ibv(), &fake);

    EXPECT_EQ(hipObj::setIbvForTest(previous), &fake);
    EXPECT_NE(&hipObj::ibv(), &fake);
}

// ---- IBV function table seam -------------------------------------

namespace {

    struct IbvCallLog {
        int create_cq_calls    = 0;
        int create_qp_calls    = 0;
        int destroy_cq_calls   = 0;
        int destroy_qp_calls   = 0;
        int dealloc_pd_calls   = 0;
        int close_device_calls = 0;
        int modify_qp_calls    = 0;
        int modify_qp_mask     = 0;
        /* From the last modify_qp() */
        unsigned int qp_access_flags = 0;
    };

    IbvCallLog g_calls;

    struct ibv_cq *fakeCreateCq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int)
    {
        ++g_calls.create_cq_calls;
        return reinterpret_cast<struct ibv_cq *>(0xC0FFEE);
    }

    struct ibv_qp *fakeCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
    {
        ++g_calls.create_qp_calls;
        return reinterpret_cast<struct ibv_qp *>(0x0DDba11);
    }

    int fakeDestroyCq(struct ibv_cq *)
    {
        ++g_calls.destroy_cq_calls;
        return 0;
    }

    int fakeDestroyQp(struct ibv_qp *)
    {
        ++g_calls.destroy_qp_calls;
        return 0;
    }

    int fakeDeallocPd(struct ibv_pd *)
    {
        ++g_calls.dealloc_pd_calls;
        return 0;
    }

    int fakeCloseDevice(struct ibv_context *)
    {
        ++g_calls.close_device_calls;
        return 0;
    }

    int fakeRecordingModifyQp(struct ibv_qp *, struct ibv_qp_attr *attr, int mask)
    {
        ++g_calls.modify_qp_calls;
        g_calls.modify_qp_mask  = mask;
        g_calls.qp_access_flags = attr->qp_access_flags;
        return 0;
    }

} // namespace

class IbvSeamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        hipObj::IbvFuncs funcs{};
        funcs.create_cq    = &fakeCreateCq;
        funcs.create_qp    = &fakeCreateQp;
        funcs.destroy_cq   = &fakeDestroyCq;
        funcs.destroy_qp   = &fakeDestroyQp;
        funcs.dealloc_pd   = &fakeDeallocPd;
        funcs.close_device = &fakeCloseDevice;
        funcs.modify_qp    = &fakeRecordingModifyQp;
        fakeIbv_.emplace(funcs);
        g_calls = IbvCallLog();
    }

    void TearDown() override
    {
        fakeIbv_.reset();
    }

    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
};

TEST_F(IbvSeamTest, QpCreationAndTeardownUseTheTable)
{
    hipObj::RcConnection conn;
    conn.ctx.reset(reinterpret_cast<struct ibv_context *>(0x1234));
    conn.pd.reset(reinterpret_cast<struct ibv_pd *>(0x5678));

    EXPECT_EQ(hipObj::createRcQp(conn, 16, 8, 8), 0);
    EXPECT_EQ(g_calls.create_cq_calls, 1);
    EXPECT_EQ(g_calls.create_qp_calls, 1);
    EXPECT_NE(conn.qp, nullptr);

    hipObj::closeRdmaDevice(conn);
    EXPECT_EQ(g_calls.destroy_qp_calls, 1);
    EXPECT_EQ(g_calls.destroy_cq_calls, 1);
    EXPECT_EQ(g_calls.dealloc_pd_calls, 1);
    EXPECT_EQ(g_calls.close_device_calls, 1);
    EXPECT_EQ(conn.qp, nullptr);
    EXPECT_EQ(conn.ctx, nullptr);
}

TEST_F(IbvSeamTest, InitTransitionGrantsRemoteReadAndWrite)
{
    hipObj::RcConnection conn;
    conn.ctx.reset(reinterpret_cast<struct ibv_context *>(0x1234));
    conn.pd.reset(reinterpret_cast<struct ibv_pd *>(0x5678));
    ASSERT_EQ(hipObj::createRcQp(conn, 16, 8, 8), 0);

    ASSERT_EQ(hipObj::transitionQpToInit(conn), 0);
    EXPECT_EQ(g_calls.modify_qp_calls, 1);
    EXPECT_NE(g_calls.modify_qp_mask & IBV_QP_ACCESS_FLAGS, 0);
    /* The peer reads the buffer for a PUT and writes it for a GET, and
     * nothing else */
    EXPECT_EQ(g_calls.qp_access_flags,
              static_cast<unsigned int>(IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
}

namespace {

    struct ApiTransferLog {
        int sendRequestCalls = 0;
        int recvReplyCalls   = 0;
    };

    ApiTransferLog g_apiTransferLog;

    struct ibv_device *g_fakeIbvDevices[] = {reinterpret_cast<struct ibv_device *>(0x1), nullptr};

    struct ibv_device **fakeGetDeviceList(int *num_devices)
    {
        if (num_devices) {
            *num_devices = 1;
        }
        return g_fakeIbvDevices;
    }

    void fakeFreeDeviceList(struct ibv_device **)
    {
    }

    struct ibv_context *fakeOpenDevice(struct ibv_device *)
    {
        return reinterpret_cast<struct ibv_context *>(0x2);
    }

    struct ibv_pd *fakeAllocPd(struct ibv_context *)
    {
        return reinterpret_cast<struct ibv_pd *>(0x3);
    }

    int fakeQueryPort(struct ibv_context *, uint8_t, struct ibv_port_attr *attr)
    {
        /* Like the real ibv_query_port, fill every field. Callers do not
         * initialize the struct, so gid_tbl_len would otherwise be stack
         * garbage and drive the GID scan for billions of iterations. */
        if (attr) {
            hipObj::zeroObject(*attr);
        }
        return 0;
    }

    int fakeQueryGid(struct ibv_context *, uint8_t, int, union ibv_gid *gid)
    {
        if (gid) {
            hipObj::zeroObject(*gid);
        }
        return 0;
    }

    const char *fakeGetDeviceName(struct ibv_device *)
    {
        return "mlx5_0";
    }

    struct ibv_mr *fakeTransferRegisterMr(struct ibv_pd *, void *addr, size_t, int)
    {
        auto *mr = static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
        if (mr) {
            mr->addr = addr;
            mr->rkey = 0x1234;
        }
        return mr;
    }

    struct ibv_mr *fakeTransferRegisterMrIova2(struct ibv_pd *pd, void *addr, size_t size, uintptr_t,
                                               int access)
    {
        return fakeTransferRegisterMr(pd, addr, size, access);
    }

    int fakeTransferDeregisterMr(struct ibv_mr *mr)
    {
        std::free(mr);
        return 0;
    }

    struct ibv_cq *fakeTransferCreateCq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int)
    {
        return reinterpret_cast<struct ibv_cq *>(0x4);
    }

    struct ibv_qp *fakeTransferCreateQp(struct ibv_pd *, struct ibv_qp_init_attr *)
    {
        auto *qp = static_cast<struct ibv_qp *>(std::calloc(1, sizeof(struct ibv_qp)));
        if (qp) {
            qp->qp_num = 0x55;
        }
        return qp;
    }

    int fakeModifyQp(struct ibv_qp *, struct ibv_qp_attr *, int)
    {
        return 0;
    }

    int fakePollCq(struct ibv_cq *, int, struct ibv_wc *wc)
    {
        if (wc) {
            hipObj::zeroObject(*wc);
            wc->status = IBV_WC_SUCCESS;
        }
        return 1;
    }

    int fakeTransferDestroyQp(struct ibv_qp *qp)
    {
        std::free(qp);
        return 0;
    }

    int fakeSendRequest(void *, const char *, size_t)
    {
        ++g_apiTransferLog.sendRequestCalls;
        return 0;
    }

    int fakeRecvReply(void *, char *reply, size_t *replyLen)
    {
        static constexpr auto kReply = std::to_array("200");
        ++g_apiTransferLog.recvReplyCalls;
        if (!replyLen || *replyLen < kReply.size()) {
            return -1;
        }
        std::ranges::copy(kReply, hipObj::spanFromPointer(reply, *replyLen).begin());
        *replyLen = kReply.size();
        return 0;
    }

    class InitAndTransferTest : public ::testing::Test {
    protected:
        void SetUp() override
        {
            savedState_      = hipObj::setStateForTest(&state_);
            savedEnumerator_ = hipObj::setNicEnumerator(&enumerator_);
            enumerator_.nics_.clear();

            hipDevice_.deviceSyncErr = hipErrorNoDevice;
            fakeHip_.emplace(hip_);

            hipObj::IbvFuncs funcs{};
            funcs.get_device_list  = &fakeGetDeviceList;
            funcs.free_device_list = &fakeFreeDeviceList;
            funcs.open_device      = &fakeOpenDevice;
            funcs.close_device     = &fakeCloseDevice;
            funcs.get_device_name  = &fakeGetDeviceName;
            funcs.query_port       = &fakeQueryPort;
            funcs.query_gid        = &fakeQueryGid;
            funcs.alloc_pd         = &fakeAllocPd;
            funcs.dealloc_pd       = &fakeDeallocPd;
            funcs.reg_mr_iova2     = &fakeTransferRegisterMrIova2;
            funcs.dereg_mr         = &fakeTransferDeregisterMr;
            funcs.create_cq        = &fakeTransferCreateCq;
            funcs.destroy_cq       = &fakeDestroyCq;
            funcs.create_qp        = &fakeTransferCreateQp;
            funcs.modify_qp        = &fakeModifyQp;
            funcs.destroy_qp       = &fakeTransferDestroyQp;
            funcs.poll_cq          = &fakePollCq;
            fakeIbv_.emplace(funcs);

            g_apiTransferLog = {};
            g_calls          = IbvCallLog();
        }

        void TearDown() override
        {
            (void)hipObjShutdown();
            fakeIbv_.reset();
            fakeHip_.reset();
            hipObj::setNicEnumerator(savedEnumerator_);
            hipObj::setStateForTest(savedState_);
        }

        hipObjConfig_t makeConfig()
        {
            hipObjConfig_t config{};
            config.gpuDevice = -1;
            config.nicHint   = "mlx5_0";
            return config;
        }

        hipObj::DriverState                      state_{};
        hipObj::DriverState                     *savedState_ = nullptr;
        FakeNicEnumerator                        enumerator_{};
        hipObj::NicEnumerator                   *savedEnumerator_ = nullptr;
        std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
        hipObjTest::FakeDevice                   hipDevice_{};
        hipObjTest::FakeHip                      hip_{hipDevice_};
        std::optional<hipObjTest::ScopedFakeHip> fakeHip_{};
    };

    TEST_F(InitAndTransferTest, InitAllowsGpuLessHostModeWithNicHint)
    {
        hipObjConfig_t config = makeConfig();

        hipObjError_t err = hipObjInit(&config);

        EXPECT_EQ(err.opError, hipObjSuccess);
        EXPECT_TRUE(state_.initialized);
        EXPECT_EQ(state_.gpuDevice, -1);
        EXPECT_EQ(state_.nicHint, "mlx5_0");
    }

    TEST_F(InitAndTransferTest, HostTransfersSkipDeviceSynchronizeWithoutGpu)
    {
        hipObjConfig_t config = makeConfig();
        ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

        char hostBuf[32]{};
        ASSERT_EQ(hipObjBufRegisterHost(hostBuf, sizeof(hostBuf)).opError, hipObjSuccess);

        hipObjOps_t ops{};
        ops.sendRequest = &fakeSendRequest;
        ops.recvReply   = &fakeRecvReply;

        hipObjError_t err = hipObjGet(nullptr, hostBuf, sizeof(hostBuf), 0, &ops, nullptr);

        EXPECT_EQ(err.opError, hipObjSuccess);
        EXPECT_EQ(g_apiTransferLog.sendRequestCalls, 1);
        EXPECT_EQ(g_apiTransferLog.recvReplyCalls, 1);
        EXPECT_EQ(hipDevice_.log.deviceSynchronize, 0);
    }

    TEST_F(InitAndTransferTest, GpuRegisteredTransfersStillSynchronize)
    {
        hipObjConfig_t config = makeConfig();
        ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

        hipDevice_.deviceSyncErr = hipSuccess;

        void *gpuBuf = reinterpret_cast<void *>(0x1000);
        ASSERT_EQ(hipObjBufRegister(gpuBuf, 64).opError, hipObjSuccess);

        hipObjOps_t ops{};
        ops.sendRequest = &fakeSendRequest;
        ops.recvReply   = &fakeRecvReply;

        hipObjError_t err = hipObjPut(nullptr, gpuBuf, 64, 0, &ops, nullptr);

        EXPECT_EQ(err.opError, hipObjSuccess);
        EXPECT_EQ(hipDevice_.log.deviceSynchronize, 1);
    }

} // namespace

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
