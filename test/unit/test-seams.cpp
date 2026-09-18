/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the three test seams (HIP runtime, NIC topology,
 * driver state). All run without GPU or RDMA hardware by substituting
 * fakes through the seam accessors.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hip-seam.h"
#include "hipobj.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "rdma-topology.h"
#include "state.h"
#include "transport.h"

namespace {

// ---- HIP seam ----------------------------------------------------

hipError_t fakeGetDevice(int* device) {
  *device = 7;
  return hipSuccess;
}

hipError_t fakeDeviceGetPCIBusId(char* bus_id, int len, int device) {
  (void)device;
  snprintf(bus_id, static_cast<size_t>(len), "0000:42:00.0");
  return hipSuccess;
}

hipError_t fakeDeviceSynchronize() {
  return hipSuccess;
}

hipError_t fakeGetDeviceNoDevice(int*) {
  return hipErrorNoDevice;
}

class HipSeamTest : public ::testing::Test {
protected:
  void SetUp() override {
    saved_ = hipObj::hipOps();
    hipObj::HipOps ops;
    ops.hipGetDevice = &fakeGetDevice;
    ops.hipDeviceGetPCIBusId = &fakeDeviceGetPCIBusId;
    ops.hipDeviceSynchronize = &fakeDeviceSynchronize;
    hipObj::hipOps() = ops;
  }

  void TearDown() override {
    hipObj::hipOps() = saved_;
  }

  hipObj::HipOps saved_;
};

TEST_F(HipSeamTest, SubstitutedFunctionsAreCalled) {
  int device = -1;
  EXPECT_EQ(hipObj::hipOps().hipGetDevice(&device), hipSuccess);
  EXPECT_EQ(device, 7);

  char bus_id[32] = {0};
  EXPECT_EQ(hipObj::hipOps().hipDeviceGetPCIBusId(bus_id, sizeof(bus_id), 0),
            hipSuccess);
  EXPECT_STREQ(bus_id, "0000:42:00.0");
}

// ---- NIC topology seam -------------------------------------------

class FakeNicEnumerator : public hipObj::NicEnumerator {
public:
  std::vector<hipObj::NicInfo> Enumerate(const char* hca_list) override {
    last_hca_list_ = hca_list ? hca_list : "";
    return nics_;
  }

  std::vector<hipObj::NicInfo> nics_;
  std::string last_hca_list_;
};

class NicSeamTest : public ::testing::Test {
protected:
  void SetUp() override {
    previous_ = hipObj::setNicEnumerator(&fake_);
    saved_hip_ = hipObj::hipOps();
    hipObj::HipOps ops = saved_hip_;
    ops.hipDeviceGetPCIBusId = &fakeDeviceGetPCIBusId;
    hipObj::hipOps() = ops;
  }

  void TearDown() override {
    hipObj::setNicEnumerator(previous_);
    hipObj::hipOps() = saved_hip_;
  }

  FakeNicEnumerator fake_;
  hipObj::NicEnumerator* previous_ = nullptr;
  hipObj::HipOps saved_hip_;
};

TEST_F(NicSeamTest, ClosestBusIdWins) {
  fake_.nics_ = {
    {"mlx5_0", "0000:18:00.0", 0, 1},
    {"mlx5_1", "0000:43:00.0", 1, 1},
    {"mlx5_2", "0000:11:00.0", 0, 1},
  };

  const char* dev_name = nullptr;
  int idx = hipObj::GetClosestNicToGpu(0, nullptr, &dev_name);
  // GPU bus 0000:42:00.0: distance 1 for 43, larger for the others.
  EXPECT_EQ(idx, 1);
  ASSERT_NE(dev_name, nullptr);
  EXPECT_STREQ(dev_name, "mlx5_1");
}

TEST_F(NicSeamTest, EmptyEnumerationFails) {
  const char* dev_name = nullptr;
  EXPECT_EQ(hipObj::GetClosestNicToGpu(0, nullptr, &dev_name), -1);
}

TEST_F(NicSeamTest, HcaFilterIsForwarded) {
  fake_.nics_ = {{"mlx5_0", "0000:18:00.0", 0, 1}};
  hipObj::GetClosestNicToGpu(0, "mlx5_0,mlx5_1", nullptr);
  EXPECT_EQ(fake_.last_hca_list_, "mlx5_0,mlx5_1");
}

// ---- Driver state seam -------------------------------------------

class StateSeamTest : public ::testing::Test {
protected:
  void TearDown() override {
    hipObj::setStateForTest(nullptr);
  }
};

TEST_F(StateSeamTest, OverrideRedirectsGetState) {
  hipObj::DriverState fresh;
  fresh.initialized = true;
  fresh.gpuDevice = 3;
  fresh.endpoint = "http://example";

  hipObj::DriverState* previous = hipObj::setStateForTest(&fresh);
  EXPECT_EQ(previous, nullptr);

  hipObj::DriverState& state = hipObj::getState();
  EXPECT_EQ(&state, &fresh);
  EXPECT_TRUE(state.initialized);
  EXPECT_EQ(state.gpuDevice, 3);

  EXPECT_EQ(hipObj::setStateForTest(nullptr), &fresh);
}

// ---- IBV function table seam -------------------------------------

namespace {

struct IbvCallLog {
  int create_cq_calls = 0;
  int create_qp_calls = 0;
  int destroy_cq_calls = 0;
  int destroy_qp_calls = 0;
  int dealloc_pd_calls = 0;
  int close_device_calls = 0;
};

IbvCallLog g_calls;

struct ibv_cq* fakeCreateCq(struct ibv_context*, int, void*,
                            struct ibv_comp_channel*, int) {
  ++g_calls.create_cq_calls;
  return reinterpret_cast<struct ibv_cq*>(0xC0FFEE);
}

struct ibv_qp* fakeCreateQp(struct ibv_pd*, struct ibv_qp_init_attr*) {
  ++g_calls.create_qp_calls;
  return reinterpret_cast<struct ibv_qp*>(0x0DDba11);
}

int fakeDestroyCq(struct ibv_cq*) {
  ++g_calls.destroy_cq_calls;
  return 0;
}

int fakeDestroyQp(struct ibv_qp*) {
  ++g_calls.destroy_qp_calls;
  return 0;
}

int fakeDeallocPd(struct ibv_pd*) {
  ++g_calls.dealloc_pd_calls;
  return 0;
}

int fakeCloseDevice(struct ibv_context*) {
  ++g_calls.close_device_calls;
  return 0;
}

} // namespace

class IbvSeamTest : public ::testing::Test {
protected:
  using Funcs = hipObj::IbvFuncs;

  void SetUp() override {
    auto& funcs = hipObj::ibv.funcsForTest();
    saved_ = funcs;
    funcs.create_cq = &fakeCreateCq;
    funcs.create_qp = &fakeCreateQp;
    funcs.destroy_cq = &fakeDestroyCq;
    funcs.destroy_qp = &fakeDestroyQp;
    funcs.dealloc_pd = &fakeDeallocPd;
    funcs.close_device = &fakeCloseDevice;
    g_calls = IbvCallLog();
  }

  void TearDown() override {
    hipObj::ibv.funcsForTest() = saved_;
  }

  Funcs saved_ = {};
};

TEST_F(IbvSeamTest, QpCreationAndTeardownUseTheTable) {
  hipObj::RcConnection conn;
  conn.ctx = reinterpret_cast<struct ibv_context*>(0x1234);
  conn.pd = reinterpret_cast<struct ibv_pd*>(0x5678);

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

namespace {

struct ApiTransferLog {
  int sendRequestCalls = 0;
  int recvReplyCalls = 0;
  int synchronizeCalls = 0;
};

ApiTransferLog g_apiTransferLog;

struct ibv_device* g_fakeIbvDevices[] = {reinterpret_cast<struct ibv_device*>(
                                           0x1),
                                         nullptr};

struct ibv_device** fakeGetDeviceList(int* num_devices) {
  if (num_devices) {
    *num_devices = 1;
  }
  return g_fakeIbvDevices;
}

void fakeFreeDeviceList(struct ibv_device**) {
}

struct ibv_context* fakeOpenDevice(struct ibv_device*) {
  return reinterpret_cast<struct ibv_context*>(0x2);
}

struct ibv_pd* fakeAllocPd(struct ibv_context*) {
  return reinterpret_cast<struct ibv_pd*>(0x3);
}

int fakeQueryPort(struct ibv_context*, uint8_t, struct ibv_port_attr*) {
  return 0;
}

int fakeQueryGid(struct ibv_context*, uint8_t, int, union ibv_gid* gid) {
  if (gid) {
    std::memset(gid, 0, sizeof(*gid));
  }
  return 0;
}

const char* fakeGetDeviceName(struct ibv_device*) {
  return "mlx5_0";
}

struct ibv_mr* fakeTransferRegisterMr(struct ibv_pd*, void* addr, size_t, int) {
  auto* mr = static_cast<struct ibv_mr*>(std::calloc(1, sizeof(struct ibv_mr)));
  if (mr) {
    mr->addr = addr;
    mr->rkey = 0x1234;
  }
  return mr;
}

struct ibv_mr* fakeTransferRegisterMrIova2(struct ibv_pd* pd, void* addr,
                                           size_t size, uintptr_t, int access) {
  return fakeTransferRegisterMr(pd, addr, size, access);
}

int fakeTransferDeregisterMr(struct ibv_mr* mr) {
  std::free(mr);
  return 0;
}

struct ibv_cq* fakeTransferCreateCq(struct ibv_context*, int, void*,
                                    struct ibv_comp_channel*, int) {
  return reinterpret_cast<struct ibv_cq*>(0x4);
}

struct ibv_qp* fakeTransferCreateQp(struct ibv_pd*, struct ibv_qp_init_attr*) {
  auto* qp = static_cast<struct ibv_qp*>(std::calloc(1, sizeof(struct ibv_qp)));
  if (qp) {
    qp->qp_num = 0x55;
  }
  return qp;
}

int fakeModifyQp(struct ibv_qp*, struct ibv_qp_attr*, int) {
  return 0;
}

int fakePollCq(struct ibv_cq*, int, struct ibv_wc* wc) {
  if (wc) {
    std::memset(wc, 0, sizeof(*wc));
    wc->status = IBV_WC_SUCCESS;
  }
  return 1;
}

int fakeTransferDestroyQp(struct ibv_qp* qp) {
  std::free(qp);
  return 0;
}

hipError_t fakeSynchronizeNoDevice() {
  ++g_apiTransferLog.synchronizeCalls;
  return hipErrorNoDevice;
}

hipError_t fakeSynchronizeSuccess() {
  ++g_apiTransferLog.synchronizeCalls;
  return hipSuccess;
}

int fakeSendRequest(void*, const char*, size_t) {
  ++g_apiTransferLog.sendRequestCalls;
  return 0;
}

int fakeRecvReply(void*, char* reply, size_t* replyLen) {
  static constexpr char kReply[] = "200";
  ++g_apiTransferLog.recvReplyCalls;
  if (!replyLen || *replyLen < sizeof(kReply)) {
    return -1;
  }
  std::memcpy(reply, kReply, sizeof(kReply));
  *replyLen = sizeof(kReply);
  return 0;
}

class InitAndTransferTest : public ::testing::Test {
protected:
  using Funcs = hipObj::IbvFuncs;

  void SetUp() override {
    savedState_ = hipObj::setStateForTest(&state_);
    savedEnumerator_ = hipObj::setNicEnumerator(&enumerator_);
    enumerator_.nics_.clear();

    savedHipOps_ = hipObj::hipOps();
    hipObj::HipOps ops = savedHipOps_;
    ops.hipGetDevice = &fakeGetDeviceNoDevice;
    ops.hipDeviceGetPCIBusId = &fakeDeviceGetPCIBusId;
    ops.hipDeviceSynchronize = &fakeSynchronizeNoDevice;
    hipObj::hipOps() = ops;

    auto& funcs = hipObj::ibv.funcsForTest();
    savedFuncs_ = funcs;
    funcs.get_device_list = &fakeGetDeviceList;
    funcs.free_device_list = &fakeFreeDeviceList;
    funcs.open_device = &fakeOpenDevice;
    funcs.close_device = &fakeCloseDevice;
    funcs.get_device_name = &fakeGetDeviceName;
    funcs.query_port = &fakeQueryPort;
    funcs.query_gid = &fakeQueryGid;
    funcs.alloc_pd = &fakeAllocPd;
    funcs.dealloc_pd = &fakeDeallocPd;
    funcs.reg_mr = &fakeTransferRegisterMr;
    funcs.reg_mr_iova2 = &fakeTransferRegisterMrIova2;
    funcs.dereg_mr = &fakeTransferDeregisterMr;
    funcs.create_cq = &fakeTransferCreateCq;
    funcs.destroy_cq = &fakeDestroyCq;
    funcs.create_qp = &fakeTransferCreateQp;
    funcs.modify_qp = &fakeModifyQp;
    funcs.destroy_qp = &fakeTransferDestroyQp;
    funcs.poll_cq = &fakePollCq;

    savedIbvInitialized_ = hipObj::ibv.is_initialized;
    hipObj::ibv.is_initialized = true;
    g_apiTransferLog = {};
    g_calls = IbvCallLog();
  }

  void TearDown() override {
    (void)hipObjShutdown();
    hipObj::ibv.is_initialized = savedIbvInitialized_;
    hipObj::ibv.funcsForTest() = savedFuncs_;
    hipObj::hipOps() = savedHipOps_;
    hipObj::setNicEnumerator(savedEnumerator_);
    hipObj::setStateForTest(savedState_);
  }

  hipObjConfig_t makeConfig() {
    hipObjConfig_t config = {};
    config.gpuDevice = -1;
    config.nicHint = "mlx5_0";
    return config;
  }

  hipObj::DriverState state_;
  hipObj::DriverState* savedState_ = nullptr;
  FakeNicEnumerator enumerator_;
  hipObj::NicEnumerator* savedEnumerator_ = nullptr;
  Funcs savedFuncs_ = {};
  hipObj::HipOps savedHipOps_;
  bool savedIbvInitialized_ = false;
};

TEST_F(InitAndTransferTest, InitAllowsGpuLessHostModeWithNicHint) {
  hipObjConfig_t config = makeConfig();

  hipObjError_t err = hipObjInit(&config);

  EXPECT_EQ(err.opError, hipObjSuccess);
  EXPECT_TRUE(state_.initialized);
  EXPECT_EQ(state_.gpuDevice, -1);
  EXPECT_EQ(state_.nicHint, "mlx5_0");
}

TEST_F(InitAndTransferTest, HostTransfersSkipDeviceSynchronizeWithoutGpu) {
  hipObjConfig_t config = makeConfig();
  ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

  char hostBuf[32] = {};
  ASSERT_EQ(hipObjBufRegisterHost(hostBuf, sizeof(hostBuf)).opError,
            hipObjSuccess);

  hipObjOps_t ops = {};
  ops.sendRequest = &fakeSendRequest;
  ops.recvReply = &fakeRecvReply;

  hipObjError_t err = hipObjGet(nullptr, hostBuf, sizeof(hostBuf), 0, &ops,
                                nullptr);

  EXPECT_EQ(err.opError, hipObjSuccess);
  EXPECT_EQ(g_apiTransferLog.sendRequestCalls, 1);
  EXPECT_EQ(g_apiTransferLog.recvReplyCalls, 1);
  EXPECT_EQ(g_apiTransferLog.synchronizeCalls, 0);
}

TEST_F(InitAndTransferTest, GpuRegisteredTransfersStillSynchronize) {
  hipObjConfig_t config = makeConfig();
  ASSERT_EQ(hipObjInit(&config).opError, hipObjSuccess);

  hipObj::HipOps opsTable = hipObj::hipOps();
  opsTable.hipDeviceSynchronize = &fakeSynchronizeSuccess;
  hipObj::hipOps() = opsTable;

  void* gpuBuf = reinterpret_cast<void*>(0x1000);
  ASSERT_EQ(hipObjBufRegister(gpuBuf, 64).opError, hipObjSuccess);

  hipObjOps_t ops = {};
  ops.sendRequest = &fakeSendRequest;
  ops.recvReply = &fakeRecvReply;

  hipObjError_t err = hipObjPut(nullptr, gpuBuf, 64, 0, &ops, nullptr);

  EXPECT_EQ(err.opError, hipObjSuccess);
  EXPECT_EQ(g_apiTransferLog.synchronizeCalls, 1);
}

} // namespace

} // namespace
