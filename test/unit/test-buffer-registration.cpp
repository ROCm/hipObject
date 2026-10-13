/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstdint>
#include <cstdlib>
#include <optional>

#include <gtest/gtest.h>

#include "buffer.h"
#include "fake-device.h"
#include "fake-ibv.h"
#include "hipobj-private.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

struct IbvLog {
    int   registerCalls    = 0;
    int   deregisterCalls  = 0;
    void *failRegisterAddr = nullptr;
};

IbvLog g_ibvLog;

struct ibv_mr *
makeFakeMr(void *addr)
{
    auto *mr = static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
    if (mr) {
        mr->addr = addr;
        mr->rkey = 0x1234;
    }
    return mr;
}

struct ibv_mr *
fakeRegisterMr(struct ibv_pd *, void *addr, size_t, int)
{
    ++g_ibvLog.registerCalls;
    if (addr == g_ibvLog.failRegisterAddr) {
        return nullptr;
    }
    return makeFakeMr(addr);
}

struct ibv_mr *
fakeRegisterMrIova2(struct ibv_pd *, void *addr, size_t, uintptr_t, int)
{
    return fakeRegisterMr(nullptr, addr, 0, 0);
}

int
fakeDeregisterMr(struct ibv_mr *mr)
{
    ++g_ibvLog.deregisterCalls;
    std::free(mr);
    return 0;
}

class BufferRegistrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        hipObj::IbvFuncs funcs{};
        funcs.reg_mr_iova2 = &fakeRegisterMrIova2;
        funcs.dereg_mr     = &fakeDeregisterMr;
        fakeIbv_.emplace(funcs);
        fakeHip_.emplace(hip_);

        g_ibvLog = {};
    }

    void TearDown() override
    {
        fakeIbv_.reset();
        fakeHip_.reset();
    }

    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
    hipObjTest::FakeDevice                   hipDevice_{};
    hipObjTest::FakeHip                      hip_{hipDevice_};
    std::optional<hipObjTest::ScopedFakeHip> fakeHip_{};
};

TEST_F(BufferRegistrationTest, HostRegistrationUsesCallerMemory)
{
    hipObj::BufferMap buffers;
    char              hostBuf[32]{};

    EXPECT_EQ(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf), reinterpret_cast<struct ibv_pd *>(1)), 0);
    const auto *entry = buffers.find(hostBuf);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->mr, nullptr);
    EXPECT_EQ(entry->mr->addr, hostBuf);
    EXPECT_EQ(entry->hostBuf, nullptr);
    EXPECT_EQ(entry->remoteAddr, reinterpret_cast<uint64_t>(hostBuf));
    EXPECT_EQ(entry->size, sizeof(hostBuf));
    EXPECT_FALSE(entry->isDmabuf);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 0);

    EXPECT_EQ(buffers.deregisterBuffer(hostBuf), 0);
    EXPECT_EQ(hipDevice_.log.hostFree, 0);
}

TEST_F(BufferRegistrationTest, DirectGpuRegistrationRequiresDeviceSync)
{
    hipObj::BufferMap buffers;
    void             *gpuBuf = reinterpret_cast<void *>(0x4000);

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)), 0);
    const auto *entry = buffers.find(gpuBuf);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->isDmabuf);
    EXPECT_EQ(entry->hostBuf, nullptr);
    EXPECT_EQ(entry->remoteAddr, reinterpret_cast<uint64_t>(gpuBuf));

    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), 0);
}

TEST_F(BufferRegistrationTest, GpuRegistrationFallbackOwnsAllocatedHostBuffer)
{
    hipObj::BufferMap buffers;
    void             *gpuBuf  = reinterpret_cast<void *>(0x1000);
    g_ibvLog.failRegisterAddr = gpuBuf;

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)), 0);
    const auto *entry = buffers.find(gpuBuf);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->mr, nullptr);
    EXPECT_EQ(entry->mr->addr, hipDevice_.lastHostMalloc);
    EXPECT_EQ(entry->hostBuf.get(), hipDevice_.lastHostMalloc);
    EXPECT_EQ(entry->remoteAddr, reinterpret_cast<uint64_t>(hipDevice_.lastHostMalloc));
    EXPECT_FALSE(entry->isDmabuf);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 1);

    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), 0);
    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_EQ(hipDevice_.lastHostFree, hipDevice_.lastHostMalloc);
}

TEST_F(BufferRegistrationTest, DeregisterAllFreesOwnedFallbackHostBuffers)
{
    hipObj::BufferMap buffers;
    void             *gpuBuf  = reinterpret_cast<void *>(0x2000);
    g_ibvLog.failRegisterAddr = gpuBuf;

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)), 0);
    ASSERT_NE(hipDevice_.lastHostMalloc, nullptr);

    buffers.deregisterAll();

    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_EQ(hipDevice_.lastHostFree, hipDevice_.lastHostMalloc);
    EXPECT_EQ(g_ibvLog.deregisterCalls, 1);
}

/* The public API checks these first, so the map's own checks are only
 * reached directly */
TEST_F(BufferRegistrationTest, RejectsOversizeAndDuplicateRegistrations)
{
    hipObj::BufferMap buffers;
    auto             *pd     = reinterpret_cast<struct ibv_pd *>(1);
    void             *gpuBuf = reinterpret_cast<void *>(0x3000);
    char              hostBuf[32]{};

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, hipObj::MAX_MR_SIZE + 1, pd), -1);
    EXPECT_EQ(buffers.registerHostBuffer(hostBuf, hipObj::MAX_MR_SIZE + 1, pd), -1);
    EXPECT_EQ(g_ibvLog.registerCalls, 0);

    ASSERT_EQ(buffers.registerBuffer(gpuBuf, 64, pd), 0);
    ASSERT_EQ(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf), pd), 0);
    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, pd), -1);
    EXPECT_EQ(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf), pd), -1);
    EXPECT_EQ(g_ibvLog.registerCalls, 2);
    ASSERT_NE(buffers.find(gpuBuf), nullptr);
    EXPECT_EQ(buffers.find(gpuBuf)->size, 64U);
}

TEST_F(BufferRegistrationTest, FindsOnlyRegisteredBuffers)
{
    /* A fake address: registration is faked, so nothing reads or writes
     * it, and there's no object there to do pointer arithmetic in */
    constexpr uintptr_t kGpuAddr = 0x5000;
    hipObj::BufferMap   buffers;
    void               *gpuBuf = reinterpret_cast<void *>(kGpuAddr);

    EXPECT_EQ(buffers.find(gpuBuf), nullptr);
    EXPECT_FALSE(buffers.isRegistered(gpuBuf));

    ASSERT_EQ(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)), 0);
    EXPECT_NE(buffers.find(gpuBuf), nullptr);
    EXPECT_TRUE(buffers.isRegistered(gpuBuf));
    /* Only the start of a registration finds it */
    EXPECT_EQ(buffers.find(reinterpret_cast<void *>(kGpuAddr + 1)), nullptr);

    ASSERT_EQ(buffers.deregisterBuffer(gpuBuf), 0);
    EXPECT_EQ(buffers.find(gpuBuf), nullptr);
    EXPECT_FALSE(buffers.isRegistered(gpuBuf));
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
