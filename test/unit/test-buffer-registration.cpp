/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdlib>

#include <gtest/gtest.h>

#include "buffer.h"
#include "hip-seam.h"
#include "ibv-wrapper.h"

namespace {

struct IbvLog {
  int registerCalls = 0;
  int deregisterCalls = 0;
  void* failRegisterAddr = nullptr;
};

IbvLog g_ibvLog;
int g_hostMallocCalls = 0;
int g_hostFreeCalls = 0;
void* g_lastHostMalloc = nullptr;
void* g_lastHostFree = nullptr;

struct ibv_mr* makeFakeMr(void* addr) {
  auto* mr = static_cast<struct ibv_mr*>(std::calloc(1, sizeof(struct ibv_mr)));
  if (mr) {
    mr->addr = addr;
    mr->rkey = 0x1234;
  }
  return mr;
}

struct ibv_mr* fakeRegisterMr(struct ibv_pd*, void* addr, size_t, int) {
  ++g_ibvLog.registerCalls;
  if (addr == g_ibvLog.failRegisterAddr) {
    return nullptr;
  }
  return makeFakeMr(addr);
}

struct ibv_mr* fakeRegisterMrIova2(struct ibv_pd*, void* addr, size_t,
                                   uintptr_t, int) {
  return fakeRegisterMr(nullptr, addr, 0, 0);
}

int fakeDeregisterMr(struct ibv_mr* mr) {
  ++g_ibvLog.deregisterCalls;
  std::free(mr);
  return 0;
}

hipError_t fakeHipHostMalloc(void** ptr, size_t size, unsigned int) {
  ++g_hostMallocCalls;
  *ptr = std::malloc(size);
  g_lastHostMalloc = *ptr;
  return *ptr ? hipSuccess : hipErrorOutOfMemory;
}

hipError_t fakeHipHostFree(void* ptr) {
  ++g_hostFreeCalls;
  g_lastHostFree = ptr;
  std::free(ptr);
  return hipSuccess;
}

class BufferRegistrationTest : public ::testing::Test {
protected:
  using Funcs = hipObj::IbvFuncs;

  void SetUp() override {
    auto& funcs = hipObj::ibv.funcsForTest();
    savedFuncs_ = funcs;
    funcs.reg_mr = &fakeRegisterMr;
    funcs.reg_mr_iova2 = &fakeRegisterMrIova2;
    funcs.dereg_mr = &fakeDeregisterMr;

    savedHipOps_ = hipObj::hipOps();
    hipObj::HipOps ops = savedHipOps_;
    ops.hipHostMalloc = &fakeHipHostMalloc;
    ops.hipHostFree = &fakeHipHostFree;
    hipObj::hipOps() = ops;

    g_ibvLog = {};
    g_hostMallocCalls = 0;
    g_hostFreeCalls = 0;
    g_lastHostMalloc = nullptr;
    g_lastHostFree = nullptr;
  }

  void TearDown() override {
    hipObj::ibv.funcsForTest() = savedFuncs_;
    hipObj::hipOps() = savedHipOps_;
  }

  Funcs savedFuncs_ = {};
  hipObj::HipOps savedHipOps_;
};

TEST_F(BufferRegistrationTest, HostRegistrationUsesCallerMemory) {
  hipObj::BufferMap buffers;
  char hostBuf[32] = {};

  EXPECT_EQ(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf),
                                       reinterpret_cast<struct ibv_pd*>(1)),
            0);
  ASSERT_NE(buffers.lookupMr(hostBuf), nullptr);
  EXPECT_EQ(buffers.lookupMr(hostBuf)->addr, hostBuf);
  EXPECT_FALSE(buffers.requiresDeviceSync(hostBuf));
  EXPECT_EQ(g_hostMallocCalls, 0);

  EXPECT_EQ(buffers.deregisterBuffer(hostBuf), 0);
  EXPECT_EQ(g_hostFreeCalls, 0);
}

TEST_F(BufferRegistrationTest, DirectGpuRegistrationRequiresDeviceSync) {
  hipObj::BufferMap buffers;
  void* gpuBuf = reinterpret_cast<void*>(0x4000);

  EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64,
                                   reinterpret_cast<struct ibv_pd*>(1)),
            0);
  EXPECT_TRUE(buffers.requiresDeviceSync(gpuBuf));

  EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), 0);
}

TEST_F(BufferRegistrationTest, GpuRegistrationFallbackOwnsAllocatedHostBuffer) {
  hipObj::BufferMap buffers;
  void* gpuBuf = reinterpret_cast<void*>(0x1000);
  g_ibvLog.failRegisterAddr = gpuBuf;

  EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64,
                                   reinterpret_cast<struct ibv_pd*>(1)),
            0);
  ASSERT_NE(buffers.lookupMr(gpuBuf), nullptr);
  EXPECT_EQ(buffers.lookupMr(gpuBuf)->addr, g_lastHostMalloc);
  EXPECT_FALSE(buffers.requiresDeviceSync(gpuBuf));
  EXPECT_EQ(g_hostMallocCalls, 1);

  EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), 0);
  EXPECT_EQ(g_hostFreeCalls, 1);
  EXPECT_EQ(g_lastHostFree, g_lastHostMalloc);
}

TEST_F(BufferRegistrationTest, DeregisterAllFreesOwnedFallbackHostBuffers) {
  hipObj::BufferMap buffers;
  void* gpuBuf = reinterpret_cast<void*>(0x2000);
  g_ibvLog.failRegisterAddr = gpuBuf;

  EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64,
                                   reinterpret_cast<struct ibv_pd*>(1)),
            0);
  ASSERT_NE(g_lastHostMalloc, nullptr);

  buffers.deregisterAll();

  EXPECT_EQ(g_hostFreeCalls, 1);
  EXPECT_EQ(g_lastHostFree, g_lastHostMalloc);
  EXPECT_EQ(g_ibvLog.deregisterCalls, 1);
}

} // namespace
