/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the test seams (HIP runtime, NIC topology, driver
 * state, libibverbs). All run without GPU or RDMA hardware by
 * substituting fakes through the seam accessors.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

/* What a FakeNicTopology answers with, and what it was asked */
struct TopologyState {
    std::vector<hipObj::NicInfo> nics{};
    std::vector<int>             roceVersions{}; /* by GID index; -1 past the end */
    std::string                  lastHcaList{};
    std::string                  lastDevName{};
    int                          lastPortNum = -1;
};

class FakeNicTopology : public hipObj::NicTopology {
public:
    explicit FakeNicTopology(TopologyState &state) noexcept : state_(&state)
    {
    }
    ~FakeNicTopology() override = default;

    FakeNicTopology(const FakeNicTopology &)            = delete;
    FakeNicTopology &operator=(const FakeNicTopology &) = delete;
    FakeNicTopology(FakeNicTopology &&)                 = delete;
    FakeNicTopology &operator=(FakeNicTopology &&)      = delete;

    std::vector<hipObj::NicInfo> enumerate(const char *hca_list) const override
    {
        state_->lastHcaList = hca_list ? hca_list : "";
        return state_->nics;
    }

    int roceVersion(const char *devName, uint8_t portNum, int gidIndex) const override
    {
        state_->lastDevName = devName;
        state_->lastPortNum = portNum;
        if (gidIndex < 0 || std::cmp_greater_equal(gidIndex, state_->roceVersions.size())) {
            return -1;
        }
        return state_->roceVersions[static_cast<size_t>(gidIndex)];
    }

private:
    /* A pointer, so the const methods can change what it points to */
    TopologyState *state_;
};

class NicSeamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fakeTopology_.emplace(topology_);
        device_.gpuBusId = kGpuBusId;
        fakeHip_.emplace(hip_);
    }

    void TearDown() override
    {
        fakeHip_.reset();
        fakeTopology_.reset();
    }

    TopologyState                                 state_{};
    FakeNicTopology                               topology_{state_};
    std::optional<hipObjTest::ScopedFakeTopology> fakeTopology_{};
    hipObjTest::FakeDevice                        device_{};
    hipObjTest::FakeHip                           hip_{device_};
    std::optional<hipObjTest::ScopedFakeHip>      fakeHip_{};
};

TEST_F(NicSeamTest, ClosestBusIdWins)
{
    state_.nics = {
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
    state_.nics = {
        {"mlx5_0", "", 0, 1},
        {"mlx5_1", "", 1, 1},
    };

    EXPECT_FALSE(hipObj::GetClosestNicToGpu(0, nullptr));
}

TEST_F(NicSeamTest, FirstOfClosestWins)
{
    state_.nics = {
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
    state_.nics = {{"mlx5_0", "0000:18:00.0", 0, 1}};
    hipObj::GetClosestNicToGpu(0, "mlx5_0,mlx5_1");
    EXPECT_EQ(state_.lastHcaList, "mlx5_0,mlx5_1");
}

/* A device name that /sys/class/infiniband doesn't have, so SysfsTopology
 * finds no bus id, NUMA node, or GID type for it */
constexpr std::string_view kFakeDevName = "hipobj_fake0";

/* Verbs whose one device has a three-entry GID table on each port: a
 * link-local GID, an IPv4-mapped one, and an unconfigured (all-zero) one.
 * Port 1 is down and port 2 is active. */
class TopologyVerbs : public hipObjTest::FakeVerbs {
public:
    using FakeVerbs::FakeVerbs;

    const char *get_device_name(struct ibv_device *) const override
    {
        return kFakeDevName.data();
    }

    int query_device(struct ibv_context *context, struct ibv_device_attr *attr) const override
    {
        const int result = FakeVerbs::query_device(context, attr);
        if (result == 0) {
            attr->phys_port_cnt = 2;
        }
        return result;
    }

    int query_port(struct ibv_context *context, uint8_t portNum, struct ibv_port_attr *attr) const override
    {
        const int result = FakeVerbs::query_port(context, portNum, attr);
        if (result == 0) {
            attr->state       = portNum == 2 ? IBV_PORT_ACTIVE : IBV_PORT_DOWN;
            attr->gid_tbl_len = 3;
        }
        return result;
    }

    int query_gid(struct ibv_context *context, uint8_t portNum, int index, union ibv_gid *gid) const override
    {
        const int result = FakeVerbs::query_gid(context, portNum, index, gid);
        if (result != 0) {
            return result;
        }
        const std::span<uint8_t> raw(gid->raw);
        if (index == 0) {
            raw[0]  = 0xfe;
            raw[1]  = 0x80;
            raw[15] = 0x01;
        }
        else if (index == 1) {
            raw[10] = 0xff;
            raw[11] = 0xff;
            raw[12] = 192;
            raw[13] = 0;
            raw[14] = 2;
            raw[15] = 1;
        }
        return 0;
    }
};

/* The fake verbs, and a context for the device they have, whose name
 * SelectBestGid() reads */
class GidSeamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::ranges::copy(kFakeDevName, std::span(device_.name).begin());
        context_.device = &device_;
        fakeTopology_.emplace(topology_);
        fakeIbv_.emplace(verbs_);
    }

    void TearDown() override
    {
        fakeIbv_.reset();
        fakeTopology_.reset();
    }

    struct ibv_device                             device_ {};
    struct ibv_context                            context_ {};
    TopologyState                                 state_{};
    FakeNicTopology                               topology_{state_};
    std::optional<hipObjTest::ScopedFakeTopology> fakeTopology_{};
    hipObjTest::FakeDevice                        fakeDevice_{};
    TopologyVerbs                                 verbs_{fakeDevice_};
    std::optional<hipObjTest::ScopedFakeIbv>      fakeIbv_{};
};

TEST_F(GidSeamTest, RoceVersionsComeFromTheTopology)
{
    /* As RoCE v2 GIDs, the IPv4-mapped one is preferred */
    state_.roceVersions = {2, 2, 2};
    EXPECT_EQ(hipObj::SelectBestGid(&context_, 2), 1);
    EXPECT_EQ(state_.lastDevName, kFakeDevName);
    EXPECT_EQ(state_.lastPortNum, 2);

    /* A RoCE v2 GID is preferred to a RoCE v1 one */
    state_.roceVersions = {2, 1, 2};
    EXPECT_EQ(hipObj::SelectBestGid(&context_, 2), 0);
}

TEST_F(GidSeamTest, NoGidWithAKnownRoceVersion)
{
    EXPECT_EQ(hipObj::SelectBestGid(&context_, 2), -1);

    state_.roceVersions = {0, 0, 0};
    EXPECT_EQ(hipObj::SelectBestGid(&context_, 2), -1);
}

TEST_F(GidSeamTest, PortQueryFails)
{
    fakeDevice_.fail.queryPort = true;
    state_.roceVersions        = {2, 2, 2};
    EXPECT_EQ(hipObj::SelectBestGid(&context_, 2), -1);
}

TEST_F(GidSeamTest, UnreadableGidsAreSkipped)
{
    fakeDevice_.fail.queryGid = true;
    state_.roceVersions       = {2, 2, 2};
    EXPECT_EQ(hipObj::SelectBestGid(&context_, 2), -1);
}

/* SysfsTopology over the fake verbs. The owning pointers it uses release
 * what it opens through ibv(), so the fake verbs are installed too. */
class SysfsTopologyTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fakeIbv_.emplace(verbs_);
    }

    void TearDown() override
    {
        fakeIbv_.reset();
    }

    /* The NIC SysfsTopology reports for the fake device */
    static hipObj::NicInfo fakeNic()
    {
        hipObj::NicInfo nic;
        nic.dev_name = kFakeDevName;
        nic.port_num = 2;
        return nic;
    }

    static void expectFakeNic(const std::vector<hipObj::NicInfo> &nics)
    {
        ASSERT_EQ(nics.size(), 1U);
        const hipObj::NicInfo expected = fakeNic();
        EXPECT_EQ(nics[0].dev_name, expected.dev_name);
        EXPECT_EQ(nics[0].pcie_bus_id, expected.pcie_bus_id);
        EXPECT_EQ(nics[0].numa_node, expected.numa_node);
        EXPECT_EQ(nics[0].port_num, expected.port_num);
    }

    hipObjTest::FakeDevice                   device_{};
    TopologyVerbs                            verbs_{device_};
    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
    const hipObj::SysfsTopology              topology_{verbs_};
};

TEST_F(SysfsTopologyTest, ListsTheFirstActivePort)
{
    expectFakeNic(topology_.enumerate(nullptr));
    EXPECT_EQ(device_.log.openDevice, 1);
    EXPECT_EQ(device_.log.closeDevice, 1);
}

TEST_F(SysfsTopologyTest, FiltersByHcaList)
{
    expectFakeNic(topology_.enumerate(""));
    expectFakeNic(topology_.enumerate("mlx5_0, \thipobj_fake0"));
    EXPECT_TRUE(topology_.enumerate("mlx5_0,mlx5_1").empty());
    EXPECT_TRUE(topology_.enumerate("hipobj_fake").empty());

    /* The filtered-out devices weren't opened */
    EXPECT_EQ(device_.log.openDevice, 2);
    EXPECT_EQ(device_.log.closeDevice, 2);
}

TEST_F(SysfsTopologyTest, SkipsDevicesItCantQuery)
{
    device_.fail.getDeviceList = true;
    EXPECT_TRUE(topology_.enumerate(nullptr).empty());
    device_.fail.getDeviceList = false;

    device_.fail.openDevice = true;
    EXPECT_TRUE(topology_.enumerate(nullptr).empty());
    device_.fail.openDevice = false;

    device_.fail.queryDevice = true;
    EXPECT_TRUE(topology_.enumerate(nullptr).empty());
    device_.fail.queryDevice = false;

    device_.fail.queryPort = true;
    EXPECT_TRUE(topology_.enumerate(nullptr).empty());

    /* Every device that was opened was closed */
    EXPECT_EQ(device_.log.openDevice, 2);
    EXPECT_EQ(device_.log.closeDevice, 2);
}

TEST_F(SysfsTopologyTest, NoRoceVersionWithoutASysfsEntry)
{
    EXPECT_EQ(topology_.roceVersion(kFakeDevName.data(), 2, 0), -1);
}

/* Without a topology installed, NIC selection uses a SysfsTopology over
 * ibv() */
TEST_F(SysfsTopologyTest, NicSelectionDefaultsToSysfs)
{
    hipObjTest::FakeDevice    hipDevice{};
    const hipObjTest::FakeHip hip(hipDevice);
    hipDevice.gpuBusId = kGpuBusId;
    const hipObjTest::ScopedFakeHip fakeHip(hip);

    /* The fake device has no bus id, so it's not chosen */
    EXPECT_FALSE(hipObj::GetClosestNicToGpu(0, nullptr));
    EXPECT_EQ(device_.log.openDevice, 1);
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

// ---- libibverbs seam --------------------------------------------

TEST(IbvSeamTest, OverrideRedirectsIbv)
{
    hipObjTest::FakeDevice      device{};
    const hipObjTest::FakeVerbs fake(device);

    const hipObj::Verbs *previous = hipObj::setIbvForTest(&fake);
    EXPECT_EQ(previous, nullptr);
    EXPECT_TRUE(hipObj::ibvAvailable());
    EXPECT_EQ(&hipObj::ibv(), &fake);

    int numDevices = 0;
    EXPECT_NE(hipObj::ibv().get_device_list(&numDevices), nullptr);
    EXPECT_EQ(numDevices, 1);
    device.fail.getDeviceList = true;
    EXPECT_EQ(hipObj::ibv().get_device_list(&numDevices), nullptr);

    EXPECT_EQ(hipObj::setIbvForTest(previous), &fake);
    /* Without fakes, ibv() loads libibverbs, if it can */
    if (hipObj::ibvAvailable()) {
        EXPECT_NE(&hipObj::ibv(), &fake);
    }
}

class FakeVerbsSeamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fakeIbv_.emplace(verbs_);
    }

    void TearDown() override
    {
        fakeIbv_.reset();
    }

    hipObjTest::FakeDevice                   device_{};
    hipObjTest::FakeVerbs                    verbs_{device_};
    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
};

TEST_F(FakeVerbsSeamTest, QpCreationAndTeardownUseTheSeam)
{
    hipObj::RcConnection conn;
    conn.ctx.reset(reinterpret_cast<struct ibv_context *>(0x1234));
    conn.pd.reset(reinterpret_cast<struct ibv_pd *>(0x5678));

    EXPECT_EQ(hipObj::createRcQp(conn, 16, 8, 8), 0);
    EXPECT_EQ(device_.log.createCq, 1);
    EXPECT_EQ(device_.log.createQp, 1);
    EXPECT_NE(conn.qp, nullptr);

    hipObj::closeRdmaDevice(conn);
    EXPECT_EQ(device_.log.destroyQp, 1);
    EXPECT_EQ(device_.log.destroyCq, 1);
    EXPECT_EQ(device_.log.deallocPd, 1);
    EXPECT_EQ(device_.log.closeDevice, 1);
    EXPECT_EQ(conn.qp, nullptr);
    EXPECT_EQ(conn.ctx, nullptr);
}

TEST_F(FakeVerbsSeamTest, InitTransitionGrantsRemoteReadAndWrite)
{
    hipObj::RcConnection conn;
    conn.ctx.reset(reinterpret_cast<struct ibv_context *>(0x1234));
    conn.pd.reset(reinterpret_cast<struct ibv_pd *>(0x5678));
    ASSERT_EQ(hipObj::createRcQp(conn, 16, 8, 8), 0);

    ASSERT_EQ(hipObj::transitionQpToInit(conn), 0);
    ASSERT_EQ(device_.modifyQpCount, 1U);
    const hipObjTest::ModifyQpCall &call = device_.modifyQp[0];
    EXPECT_NE(call.mask & IBV_QP_ACCESS_FLAGS, 0);
    /* The peer reads the buffer for a PUT and writes it for a GET, and
     * nothing else */
    EXPECT_EQ(call.attr.qp_access_flags,
              static_cast<unsigned int>(IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
}

namespace {

    struct ApiTransferLog {
        int sendRequestCalls = 0;
        int recvReplyCalls   = 0;
    };

    ApiTransferLog g_apiTransferLog;

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
            savedState_ = hipObj::setStateForTest(&state_);
            fakeTopology_.emplace(topology_);

            hipDevice_.deviceSyncErr = hipErrorNoDevice;
            fakeHip_.emplace(hip_);

            fakeIbv_.emplace(verbs_);

            g_apiTransferLog = {};
        }

        void TearDown() override
        {
            (void)hipObjShutdown();
            fakeIbv_.reset();
            fakeHip_.reset();
            fakeTopology_.reset();
            hipObj::setStateForTest(savedState_);
        }

        hipObjConfig_t makeConfig()
        {
            hipObjConfig_t config{};
            config.gpuDevice = -1;
            config.nicHint   = "mlx5_0";
            return config;
        }

        hipObj::DriverState                           state_{};
        hipObj::DriverState                          *savedState_ = nullptr;
        TopologyState                                 topologyState_{};
        FakeNicTopology                               topology_{topologyState_};
        std::optional<hipObjTest::ScopedFakeTopology> fakeTopology_{};
        hipObjTest::FakeDevice                        hipDevice_{};
        hipObjTest::FakeVerbs                         verbs_{hipDevice_};
        std::optional<hipObjTest::ScopedFakeIbv>      fakeIbv_{};
        hipObjTest::FakeHip                           hip_{hipDevice_};
        std::optional<hipObjTest::ScopedFakeHip>      fakeHip_{};
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
