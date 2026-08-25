/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the three test seams (HIP runtime, NIC topology,
 * driver state). All run without GPU or RDMA hardware by substituting
 * fakes through the seam accessors.
 */

#include <cstdio>
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

} // namespace
