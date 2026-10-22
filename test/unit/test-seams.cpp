/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the test seams (HIP runtime, NIC topology, libibverbs)
 * and the platform built from them. The code under test reaches the
 * outside world only through the interfaces it's given, so these run
 * without GPU or RDMA hardware by giving it fakes. The tests of the system
 * platform use the real HIP runtime and libibverbs, and pass whether or
 * not there's a GPU or an RDMA device.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#ifdef HIPOBJ_CPU_RDMA
#include "hip-shim.h"
#else
#include <hip/hip_runtime_api.h>
#include <hsa/hsa_ext_amd.h>
#endif

#include "fake-device.h"
#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "rdma-topology.h"
#include "rdmacm-core.h"
#include "rdmacm-wrapper.h"
#include "require-libibverbs.h"
#include "state.h"
#include "transport.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

// ---- HIP seam ----------------------------------------------------

/* The GPU's bus ID in the NIC topology tests */
constexpr std::string_view kGpuBusId = "0000:42:00.0";

#ifndef HIPOBJ_CPU_RDMA
/* SystemHip passes each call straight to HIP or HSA, so it gets the same
 * answer as calling them directly, whether or not there's a GPU. The
 * CPU-only build has neither: hip-shim.h stands in for HIP. */
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
#endif /* HIPOBJ_CPU_RDMA */

#ifdef HIPOBJ_RDMA_WIRE_V3
/* SystemRdmaCm calls librdmacm, in a v3 build. Its event names need no
 * device; without an RDMA device, there's no event channel to create, and
 * with one, the channel it creates is real. */
TEST(RdmaCmSeamTest, SystemRdmaCmCallsLibrdmacm)
{
    const hipObj::SystemRdmaCm system;
    EXPECT_EQ(std::string_view(system.event_str(RDMA_CM_EVENT_ESTABLISHED)), "RDMA_CM_EVENT_ESTABLISHED");
    EXPECT_EQ(std::string_view(system.event_str(RDMA_CM_EVENT_CONNECT_REQUEST)),
              "RDMA_CM_EVENT_CONNECT_REQUEST");

    rdma_event_channel *channel = system.create_event_channel();
    if (channel == nullptr) {
        GTEST_SKIP() << "librdmacm has no RDMA device to create an event channel on";
    }
    EXPECT_GE(channel->fd, 0);
    rdma_cm_id *id = nullptr;
    ASSERT_EQ(system.create_id(channel, &id, nullptr, RDMA_PS_TCP), 0);
    EXPECT_EQ(id->channel, channel);
    EXPECT_EQ(system.destroy_id(id), 0);
    system.destroy_event_channel(channel);
}
#endif /* HIPOBJ_RDMA_WIRE_V3 */

// ---- NIC topology seam -------------------------------------------

/* Short names for the RoCE versions in the GID tables */
constexpr hipObj::RoceVersion kUnknown = hipObj::RoceVersion::Unknown;
constexpr hipObj::RoceVersion kV1      = hipObj::RoceVersion::V1;
constexpr hipObj::RoceVersion kV2      = hipObj::RoceVersion::V2;

/* What a FakeNicTopology answers with, and what it was asked */
struct TopologyState {
    std::vector<hipObj::NicInfo>     nics{};
    std::vector<hipObj::RoceVersion> roceVersions{}; /* by GID index; Unknown past the end */
    std::string                      lastHcaList{};
    std::string                      lastDevName{};
    int                              lastPortNum = -1;
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

    [[nodiscard]] std::vector<hipObj::NicInfo> enumerate(std::string_view hcaList) const override
    {
        state_->lastHcaList = hcaList;
        return state_->nics;
    }

    [[nodiscard]] hipObj::RoceVersion roceVersion(std::string_view devName, uint8_t portNum,
                                                  int gidIndex) const override
    {
        state_->lastDevName = devName;
        state_->lastPortNum = portNum;
        if (gidIndex < 0 || std::cmp_greater_equal(gidIndex, state_->roceVersions.size())) {
            return hipObj::RoceVersion::Unknown;
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
        device_.gpuBusId = kGpuBusId;
    }

    /* The NIC closest to GPU 0, with the fakes */
    [[nodiscard]] std::optional<hipObj::RdmaNic> closestNic(std::string_view hcaList = {}) const
    {
        return hipObj::GetClosestNicToGpu(hip_, topology_, 0, hcaList);
    }

    TopologyState          state_{};
    FakeNicTopology        topology_{state_};
    hipObjTest::FakeDevice device_{};
    hipObjTest::FakeHip    hip_{device_};
};

TEST_F(NicSeamTest, ClosestBusIdWins)
{
    state_.nics = {
        {.dev_name = "mlx5_0", .pcie_bus_id = "0000:18:00.0", .numa_node = 0, .port_num = 1},
        {.dev_name = "mlx5_1", .pcie_bus_id = "0000:43:00.0", .numa_node = 1, .port_num = 1},
        {.dev_name = "mlx5_2", .pcie_bus_id = "0000:11:00.0", .numa_node = 0, .port_num = 1},
    };

    const auto nic = closestNic();
    // GPU bus 0000:42:00.0: distance 1 for 43, larger for the others.
    ASSERT_TRUE(nic);
    EXPECT_EQ(nic->index, 1);
    EXPECT_EQ(nic->name, "mlx5_1");
}

TEST_F(NicSeamTest, EmptyEnumerationFails)
{
    EXPECT_FALSE(closestNic());
}

TEST_F(NicSeamTest, NoKnownBusIdFails)
{
    state_.nics = {
        {.dev_name = "mlx5_0", .pcie_bus_id = "", .numa_node = 0, .port_num = 1},
        {.dev_name = "mlx5_1", .pcie_bus_id = "", .numa_node = 1, .port_num = 1},
    };

    EXPECT_FALSE(closestNic());
}

/* The GPU's bus id comes from the runtime */
TEST_F(NicSeamTest, UnknownGpuBusIdFails)
{
    state_.nics      = {{.dev_name = "mlx5_0", .pcie_bus_id = "0000:43:00.0", .numa_node = 0, .port_num = 1}};
    device_.gpuBusId = {};
    EXPECT_FALSE(closestNic());
}

TEST_F(NicSeamTest, FirstOfClosestWins)
{
    state_.nics = {
        {.dev_name = "mlx5_0", .pcie_bus_id = "", .numa_node = 0, .port_num = 1},
        {.dev_name = "mlx5_1", .pcie_bus_id = "0000:43:00.0", .numa_node = 1, .port_num = 1},
        {.dev_name = "mlx5_2", .pcie_bus_id = "0000:43:00.1", .numa_node = 1, .port_num = 1},
    };

    const auto nic = closestNic();
    ASSERT_TRUE(nic);
    EXPECT_EQ(nic->index, 1);
    EXPECT_EQ(nic->name, "mlx5_1");
}

TEST_F(NicSeamTest, HcaFilterIsForwarded)
{
    state_.nics = {{.dev_name = "mlx5_0", .pcie_bus_id = "0000:18:00.0", .numa_node = 0, .port_num = 1}};
    /* Only the list the topology was given matters here */
    std::ignore = closestNic("mlx5_0,mlx5_1");
    EXPECT_EQ(state_.lastHcaList, "mlx5_0,mlx5_1");
}

/* A bus ID as long as the buffer GetClosestNicToGpu() reads it into, so
 * there's no room for a NUL after it */
constexpr std::string_view kFullBufferBusId = "000000000000000000000000:42:00.0";
static_assert(kFullBufferBusId.size() == hipObj::kPciBusIdBufLen);

/* A HIP runtime that fills the bus ID buffer with kFullBufferBusId, without
 * a NUL, and succeeds. FakeHip doesn't, since it always writes a NUL. */
class FullBufferBusIdHip : public hipObjTest::FakeHip {
public:
    using FakeHip::FakeHip;

    [[nodiscard]] hipError_t deviceGetPCIBusId(char *pciBusId, int len, int /*device*/) const override
    {
        const std::span<char> out = hipObj::spanFromPointer(pciBusId, static_cast<size_t>(len));
        if (out.size() != kFullBufferBusId.size()) {
            return hipErrorInvalidValue;
        }
        std::ranges::copy(kFullBufferBusId, out.begin());
        return hipSuccess;
    }
};

/* A bus ID that fills the buffer is used whole, and nothing past the buffer
 * is read, which AddressSanitizer would report */
TEST_F(NicSeamTest, BusIdThatFillsTheBufferIsUsedWhole)
{
    state_.nics = {
        {.dev_name = "mlx5_0", .pcie_bus_id = std::string(kGpuBusId), .numa_node = 0, .port_num = 1},
        {.dev_name = "mlx5_1", .pcie_bus_id = std::string(kFullBufferBusId), .numa_node = 1, .port_num = 1},
    };

    const FullBufferBusIdHip hip(device_);
    const auto               nic = hipObj::GetClosestNicToGpu(hip, topology_, 0, {});
    ASSERT_TRUE(nic) << "the fake fails unless the buffer is 32 bytes";
    EXPECT_EQ(nic->index, 1);
    EXPECT_EQ(nic->name, "mlx5_1");
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

    [[nodiscard]] const char *get_device_name(ibv_device * /*device*/) const override
    {
        return kFakeDevName.data();
    }

    [[nodiscard]] int query_device(ibv_context *context, ibv_device_attr *attr) const override
    {
        const int result = FakeVerbs::query_device(context, attr);
        if (result == 0) {
            attr->phys_port_cnt = 2;
        }
        return result;
    }

    [[nodiscard]] int query_port(ibv_context *context, uint8_t portNum, ibv_port_attr *attr) const override
    {
        const int result = FakeVerbs::query_port(context, portNum, attr);
        if (result == 0) {
            attr->state       = portNum == 2 ? IBV_PORT_ACTIVE : IBV_PORT_DOWN;
            attr->gid_tbl_len = 3;
        }
        return result;
    }

    [[nodiscard]] int query_gid(ibv_context *context, uint8_t portNum, int index, ibv_gid *gid) const override
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

/* TopologyVerbs whose device opens as context, whose name GID selection
 * reads */
class OpenableTopologyVerbs : public TopologyVerbs {
public:
    OpenableTopologyVerbs(hipObjTest::FakeDevice &device, ibv_context &context) noexcept
        : TopologyVerbs(device), context_(&context)
    {
    }
    ~OpenableTopologyVerbs() override = default;

    OpenableTopologyVerbs(const OpenableTopologyVerbs &)            = delete;
    OpenableTopologyVerbs &operator=(const OpenableTopologyVerbs &) = delete;
    OpenableTopologyVerbs(OpenableTopologyVerbs &&)                 = delete;
    OpenableTopologyVerbs &operator=(OpenableTopologyVerbs &&)      = delete;

    [[nodiscard]] ibv_context *open_device(ibv_device *device) const override
    {
        return TopologyVerbs::open_device(device) != nullptr ? context_ : nullptr;
    }

private:
    ibv_context *context_;
};

/* The fake verbs, and a context for the device they have, whose name
 * SelectBestGid() reads */
class GidSeamTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::ranges::copy(kFakeDevName, std::span(device_.name).begin());
        context_.device = &device_;
    }

    std::optional<int> selectBestGid(uint8_t portNum)
    {
        return hipObj::SelectBestGid(verbs_, topology_, &context_, portNum);
    }

    ibv_device             device_{};
    ibv_context            context_{};
    TopologyState          state_{};
    FakeNicTopology        topology_{state_};
    hipObjTest::FakeDevice fakeDevice_{};
    OpenableTopologyVerbs  verbs_{fakeDevice_, context_};
};

TEST_F(GidSeamTest, RoceVersionsComeFromTheTopology)
{
    /* As RoCE v2 GIDs, the IPv4-mapped one is preferred */
    state_.roceVersions = {kV2, kV2, kV2};
    EXPECT_EQ(selectBestGid(2), 1);
    EXPECT_EQ(state_.lastDevName, kFakeDevName);
    EXPECT_EQ(state_.lastPortNum, 2);

    /* A RoCE v2 GID is preferred to a RoCE v1 one */
    state_.roceVersions = {kV2, kV1, kV2};
    EXPECT_EQ(selectBestGid(2), 0);
}

TEST_F(GidSeamTest, NoGidWithAKnownRoceVersion)
{
    EXPECT_EQ(selectBestGid(2), std::nullopt);

    state_.roceVersions = {kUnknown, kUnknown, kUnknown};
    EXPECT_EQ(selectBestGid(2), std::nullopt);
}

TEST_F(GidSeamTest, PortQueryFails)
{
    fakeDevice_.fail.queryPort = true;
    state_.roceVersions        = {kV2, kV2, kV2};
    EXPECT_EQ(selectBestGid(2), std::nullopt);
}

TEST_F(GidSeamTest, UnreadableGidsAreSkipped)
{
    fakeDevice_.fail.queryGid = true;
    state_.roceVersions       = {kV2, kV2, kV2};
    EXPECT_EQ(selectBestGid(2), std::nullopt);
}

/* Opening a device chooses its GID with the topology it's given */
TEST_F(GidSeamTest, OpenedDeviceHasTheGidTheTopologyPrefers)
{
    state_.roceVersions                          = {kV2, kV2, kV2};
    const std::optional<hipObj::RdmaDevice> rdma = hipObj::openRdmaDevice(verbs_, topology_, 0);
    ASSERT_TRUE(rdma);
    EXPECT_EQ(rdma->context(), &context_);
    EXPECT_EQ(rdma->portNum(), 1);
    EXPECT_EQ(state_.lastPortNum, 1);
    EXPECT_EQ(rdma->gidIndex(), 1);

    ibv_gid expected{};
    ASSERT_EQ(verbs_.query_gid(&context_, 1, 1, &expected), 0);
    EXPECT_EQ(hipObj::toGid(rdma->localGid()), hipObj::toGid(expected));
}

/* SysfsTopology over the fake verbs */
class SysfsTopologyTest : public ::testing::Test {
protected:
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

    hipObjTest::FakeDevice      device_{};
    TopologyVerbs               verbs_{device_};
    const hipObj::SysfsTopology topology_{verbs_};
};

TEST_F(SysfsTopologyTest, ListsTheFirstActivePort)
{
    expectFakeNic(topology_.enumerate({}));
    EXPECT_EQ(device_.log.openDevice, 1);
    EXPECT_EQ(device_.log.closeDevice, 1);
}

TEST_F(SysfsTopologyTest, FiltersByHcaList)
{
    expectFakeNic(topology_.enumerate("mlx5_0, \thipobj_fake0"));
    expectFakeNic(topology_.enumerate("hipobj_fake0,"));
    EXPECT_TRUE(topology_.enumerate("mlx5_0,mlx5_1").empty());
    EXPECT_TRUE(topology_.enumerate("hipobj_fake").empty());
    EXPECT_TRUE(topology_.enumerate("hipobj_fake00").empty());

    /* The filtered-out devices weren't opened */
    EXPECT_EQ(device_.log.openDevice, 2);
    EXPECT_EQ(device_.log.closeDevice, 2);
}

TEST_F(SysfsTopologyTest, SkipsDevicesItCantQuery)
{
    device_.fail.getDeviceList = true;
    EXPECT_TRUE(topology_.enumerate({}).empty());
    device_.fail.getDeviceList = false;

    device_.fail.openDevice = true;
    EXPECT_TRUE(topology_.enumerate({}).empty());
    device_.fail.openDevice = false;

    device_.fail.queryDevice = true;
    EXPECT_TRUE(topology_.enumerate({}).empty());
    device_.fail.queryDevice = false;

    device_.fail.queryPort = true;
    EXPECT_TRUE(topology_.enumerate({}).empty());

    /* Every device that was opened was closed */
    EXPECT_EQ(device_.log.openDevice, 2);
    EXPECT_EQ(device_.log.closeDevice, 2);
}

TEST_F(SysfsTopologyTest, NoRoceVersionWithoutASysfsEntry)
{
    EXPECT_EQ(topology_.roceVersion(kFakeDevName, 2, 0), hipObj::RoceVersion::Unknown);
}

TEST_F(SysfsTopologyTest, NicSelection)
{
    hipObjTest::FakeDevice    hipDevice{};
    const hipObjTest::FakeHip hip(hipDevice);
    hipDevice.gpuBusId = kGpuBusId;

    /* The fake device has no bus id, so it's not chosen */
    EXPECT_FALSE(hipObj::GetClosestNicToGpu(hip, topology_, 0, {}));
    EXPECT_EQ(device_.log.openDevice, 1);
}

// ---- Platform ----------------------------------------------------

/* A platform refers to the interfaces it's given */
TEST(PlatformTest, RefersToItsInterfaces)
{
    hipObjTest::FakeDevice             device{};
    const hipObjTest::FakeHip          hip(device);
    const hipObjTest::FakeVerbs        verbs(device);
    const hipObjTest::EmptyNicTopology topology;
    const hipObjTest::FakeRdmaCm       rdmaCm(device, verbs);

    const hipObj::Platform platform(hip, verbs, topology, &rdmaCm);
    EXPECT_EQ(&platform.hip(), &hip);
    EXPECT_EQ(&platform.verbs(), &verbs);
    EXPECT_EQ(&platform.topology(), &topology);
    EXPECT_EQ(platform.rdmaCm(), &rdmaCm);
}

/* A FakePlatform's platform refers to its fakes, and keeps them alive */
TEST(PlatformTest, FakePlatformOutlivesItsLastPlatform)
{
    hipObjTest::FakeDevice device{};
    auto                   fakes = std::make_shared<hipObjTest::FakePlatform<>>(device);
    const std::weak_ptr<hipObjTest::FakePlatform<>> watch = fakes;

    std::shared_ptr<const hipObj::Platform> platform = fakes->platform();
    EXPECT_EQ(&platform->hip(), &fakes->hip());
    EXPECT_EQ(&platform->verbs(), &fakes->verbs());
    EXPECT_EQ(&platform->topology(), &fakes->topology());
    EXPECT_EQ(platform->rdmaCm(), &fakes->rdmaCm());

    fakes.reset();
    EXPECT_FALSE(watch.expired());
    platform.reset();
    EXPECT_TRUE(watch.expired());
}

/* The system platform is built once, from the real HIP runtime,
 * libibverbs, the topology sysfs reports, and, in a v3 build, librdmacm */
TEST(PlatformTest, SystemPlatformIsTheSystem)
{
    const std::shared_ptr<const hipObj::Platform> platform = hipObj::systemPlatform();
    if (!platform) {
        ASSERT_FALSE(hipObjTest::libibverbsRequired()) << "libibverbs couldn't be loaded";
        GTEST_SKIP() << "libibverbs couldn't be loaded";
    }
    EXPECT_EQ(hipObj::systemPlatform(), platform);
    EXPECT_NE(dynamic_cast<const hipObj::SystemHip *>(&platform->hip()), nullptr);
    EXPECT_NE(dynamic_cast<const hipObj::LoadedVerbs *>(&platform->verbs()), nullptr);
    EXPECT_NE(dynamic_cast<const hipObj::SysfsTopology *>(&platform->topology()), nullptr);
#ifdef HIPOBJ_RDMA_WIRE_V3
    EXPECT_NE(dynamic_cast<const hipObj::SystemRdmaCm *>(platform->rdmaCm()), nullptr);
#else
    EXPECT_EQ(platform->rdmaCm(), nullptr);
#endif
}

/* hipObjInit() initializes with the system platform. No system has this
 * NIC, so it fails the same way with or without a GPU or libibverbs: with
 * a GPU, NIC selection finds nothing the hint allows, and either way, the
 * hinted NIC can't be opened. */
TEST(PlatformTest, InitUsesTheSystemPlatform)
{
    hipObjConfig_t config{};
    config.gpuDevice = -1;
    config.nicHint   = "hipobj_no_such_nic";
    EXPECT_EQ(hipObjInit(&config).opError, hipObjRdmaError);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
}

/* Devices on made-up objects that the fake verbs release, which code can
 * only reach through the device */
class FakeVerbsSeamTest : public ::testing::Test {
protected:
    [[nodiscard]] hipObj::RdmaDevice makeDevice() const
    {
        return {verbs_,
                hipObj::own<hipObj::IbvContextPtr>(verbs_, hipObj::addressToPointer<ibv_context *>(0x1234)),
                hipObj::own<hipObj::IbvPdPtr>(verbs_, hipObj::addressToPointer<ibv_pd *>(0x5678)),
                1,
                0,
                {},
                std::nullopt};
    }

    hipObjTest::FakeDevice device_{};
    hipObjTest::FakeVerbs  verbs_{device_};
};

TEST_F(FakeVerbsSeamTest, QpCreationAndTeardownUseTheSeam)
{
    {
        const hipObj::RdmaDevice rdma = makeDevice();
        {
            const std::optional<hipObj::RcConnection> conn = hipObj::createRcConnection(rdma, 16, 8, 8);
            ASSERT_TRUE(conn);
            EXPECT_EQ(device_.log.createCq, 1);
            EXPECT_EQ(device_.log.createQp, 1);
            EXPECT_NE(conn->qp(), nullptr);
        }

        /* Destroying the connection releases its QP and CQ */
        EXPECT_EQ(device_.log.destroyQp, 1);
        EXPECT_EQ(device_.log.destroyCq, 1);
        EXPECT_EQ(device_.log.deallocPd, 0);
    }

    /* Destroying the device releases the rest */
    EXPECT_EQ(device_.log.deallocPd, 1);
    EXPECT_EQ(device_.log.closeDevice, 1);
}

TEST_F(FakeVerbsSeamTest, InitTransitionGrantsRemoteReadAndWrite)
{
    const hipObj::RdmaDevice rdma = makeDevice();
    ASSERT_TRUE(hipObj::createRcConnection(rdma, 16, 8, 8));
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

    int fakeSendRequest(void * /*ctx*/, const char *token, size_t tokenLen)
    {
        ++g_apiTransferLog.sendRequestCalls;
        hipObjTest::noteSentToken(token, tokenLen);
        return 0;
    }

    int fakeRecvReply(void * /*ctx*/, char *reply, size_t *replyLen)
    {
        ++g_apiTransferLog.recvReplyCalls;
        // Mirrors state.h's useV1Wire(): HIPOBJ_RDMA_WIRE_V3 is a compile-time
        // choice (hipobj is built for v1 XOR v3), so this fake's reply format
        // must track whichever one hipobj_test_objects was built with here.
#ifndef HIPOBJ_RDMA_WIRE_V3
        // v1 wire format (see token.cpp's parseRdmaReplyHttpCode()): "ok" is
        // its own special-cased success reply, with no peer QP/GID/LID.
        static const std::string kReply = "ok";
#else
        // v3 wire format (see v3-wire.h): the fake device's success reply,
        // which reports the length the token named, as the real server does
        const std::string kReply = hipObjTest::successReply();
#endif
        if (replyLen == nullptr || *replyLen < kReply.size() + 1) {
            return -1;
        }
        /* The reply and its NUL, but its length is the reply's alone */
        const std::span<char> out = hipObj::spanFromPointer(reply, *replyLen);
        std::ranges::copy(kReply, out.begin());
        out[kReply.size()] = '\0';
        *replyLen          = kReply.size();
        return 0;
    }

    /* Without a GPU, so the device can't be synchronized */
    class InitAndTransferTest : public hipObjTest::FakeDeviceTest {
    protected:
        void SetUp() override
        {
            FakeDeviceTest::SetUp();
            hipObjTest::fake().deviceSyncErr = hipErrorNoDevice;
            g_apiTransferLog                 = {};
        }
    };

    TEST_F(InitAndTransferTest, InitAllowsGpuLessHostModeWithNicHint)
    {
        const hipObjError_t err = initFakes(makeConfig());

        EXPECT_EQ(err.opError, hipObjSuccess);
        const std::shared_ptr<const hipObj::Instance> instance = hipObj::currentInstance();
        ASSERT_NE(instance, nullptr);
        EXPECT_EQ(instance->config.gpuDevice, -1);
        EXPECT_EQ(instance->config.nicHint, "mlx5_0");
    }

    TEST_F(InitAndTransferTest, HostTransfersSkipDeviceSynchronizeWithoutGpu)
    {
        ASSERT_NO_FATAL_FAILURE(init());

        std::array<char, 32> hostBuf{};
        ASSERT_EQ(hipObjBufRegisterHost(hostBuf.data(), hostBuf.size()).opError, hipObjSuccess);

        hipObjOps_t ops{};
        ops.sendRequest = &fakeSendRequest;
        ops.recvReply   = &fakeRecvReply;

        const hipObjError_t err = hipObjGet(nullptr, hostBuf.data(), hostBuf.size(), 0, &ops, nullptr);

        EXPECT_EQ(err.opError, hipObjSuccess);
        EXPECT_EQ(g_apiTransferLog.sendRequestCalls, 1);
        EXPECT_EQ(g_apiTransferLog.recvReplyCalls, 1);
        EXPECT_EQ(hipObjTest::fake().log.deviceSynchronize, 0);
    }

    TEST_F(InitAndTransferTest, GpuRegisteredTransfersStillSynchronize)
    {
        ASSERT_NO_FATAL_FAILURE(init());

        hipObjTest::fake().deviceSyncErr = hipSuccess;

        void *gpuBuf = hipObj::addressToPointer<void *>(0x1000);
        ASSERT_EQ(hipObjBufRegister(gpuBuf, 64).opError, hipObjSuccess);

        hipObjOps_t ops{};
        ops.sendRequest = &fakeSendRequest;
        ops.recvReply   = &fakeRecvReply;

        const hipObjError_t err = hipObjPut(nullptr, gpuBuf, 64, 0, &ops, nullptr);

        EXPECT_EQ(err.opError, hipObjSuccess);
        EXPECT_EQ(hipObjTest::fake().log.deviceSynchronize, 1);
    }

} // namespace

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
