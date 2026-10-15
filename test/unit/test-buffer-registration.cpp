/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <cstdint>

#include <gtest/gtest.h>

#include "buffer.h"
#include "fake-device.h"
#include "hipobj-private.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* BufferMap with the fake verbs and HIP runtime */
class BufferRegistrationTest : public ::testing::Test {
protected:
    [[nodiscard]] hipObj::BufferMap makeBuffers() const
    {
        return {verbs_, hip_, {}, false};
    }

    hipObjTest::FakeDevice hipDevice_{};
    hipObjTest::FakeVerbs  verbs_{hipDevice_};
    hipObjTest::FakeHip    hip_{hipDevice_};
};

TEST_F(BufferRegistrationTest, HostRegistrationUsesCallerMemory)
{
    hipObj::BufferMap buffers = makeBuffers();
    char              hostBuf[32]{};

    EXPECT_TRUE(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf), reinterpret_cast<struct ibv_pd *>(1)));
    const auto *entry = buffers.find(hostBuf);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->mr, nullptr);
    EXPECT_EQ(entry->mr->addr, hostBuf);
    EXPECT_EQ(entry->hostBuf, nullptr);
    EXPECT_EQ(entry->remoteAddr, reinterpret_cast<uint64_t>(hostBuf));
    EXPECT_EQ(entry->size, sizeof(hostBuf));
    EXPECT_FALSE(entry->isDmabuf);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 0);

    EXPECT_TRUE(buffers.deregisterBuffer(hostBuf));
    EXPECT_EQ(hipDevice_.log.hostFree, 0);
}

TEST_F(BufferRegistrationTest, DirectGpuRegistrationRequiresDeviceSync)
{
    hipObj::BufferMap buffers = makeBuffers();
    void             *gpuBuf  = reinterpret_cast<void *>(0x4000);

    EXPECT_TRUE(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)));
    const auto *entry = buffers.find(gpuBuf);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->isDmabuf);
    EXPECT_EQ(entry->hostBuf, nullptr);
    EXPECT_EQ(entry->remoteAddr, reinterpret_cast<uint64_t>(gpuBuf));

    EXPECT_TRUE(buffers.deregisterBuffer(gpuBuf));
}

TEST_F(BufferRegistrationTest, GpuRegistrationFallbackOwnsAllocatedHostBuffer)
{
    hipObj::BufferMap buffers    = makeBuffers();
    void             *gpuBuf     = reinterpret_cast<void *>(0x1000);
    hipDevice_.fail.registerAddr = gpuBuf;

    EXPECT_TRUE(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)));
    const auto *entry = buffers.find(gpuBuf);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->mr, nullptr);
    EXPECT_EQ(entry->mr->addr, hipDevice_.lastHostMalloc);
    EXPECT_EQ(entry->hostBuf.get(), hipDevice_.lastHostMalloc);
    EXPECT_EQ(entry->remoteAddr, reinterpret_cast<uint64_t>(hipDevice_.lastHostMalloc.load()));
    EXPECT_FALSE(entry->isDmabuf);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 1);

    EXPECT_TRUE(buffers.deregisterBuffer(gpuBuf));
    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_EQ(hipDevice_.lastHostFree, hipDevice_.lastHostMalloc);
}

/* Destroying the map, as destroying the library's instance does,
 * releases what it holds */
TEST_F(BufferRegistrationTest, DestroyingFreesOwnedFallbackHostBuffers)
{
    {
        hipObj::BufferMap buffers    = makeBuffers();
        void             *gpuBuf     = reinterpret_cast<void *>(0x2000);
        hipDevice_.fail.registerAddr = gpuBuf;

        EXPECT_TRUE(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)));
        ASSERT_NE(hipDevice_.lastHostMalloc, nullptr);
        EXPECT_EQ(hipDevice_.log.hostFree, 0);
    }

    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_EQ(hipDevice_.lastHostFree, hipDevice_.lastHostMalloc);
    EXPECT_EQ(hipDevice_.log.deregMr, 1);
}

/* The public API checks these first, so the map's own checks are only
 * reached directly */
TEST_F(BufferRegistrationTest, RejectsOversizeAndDuplicateRegistrations)
{
    hipObj::BufferMap buffers = makeBuffers();
    auto             *pd      = reinterpret_cast<struct ibv_pd *>(1);
    void             *gpuBuf  = reinterpret_cast<void *>(0x3000);
    char              hostBuf[32]{};

    EXPECT_FALSE(buffers.registerBuffer(gpuBuf, hipObj::MAX_MR_SIZE + 1, pd));
    EXPECT_FALSE(buffers.registerHostBuffer(hostBuf, hipObj::MAX_MR_SIZE + 1, pd));
    EXPECT_EQ(hipDevice_.log.registerMr, 0);

    ASSERT_TRUE(buffers.registerBuffer(gpuBuf, 64, pd));
    ASSERT_TRUE(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf), pd));
    EXPECT_FALSE(buffers.registerBuffer(gpuBuf, 64, pd));
    EXPECT_FALSE(buffers.registerHostBuffer(hostBuf, sizeof(hostBuf), pd));
    EXPECT_EQ(hipDevice_.log.registerMr, 2);
    ASSERT_NE(buffers.find(gpuBuf), nullptr);
    EXPECT_EQ(buffers.find(gpuBuf)->size, 64U);
}

TEST_F(BufferRegistrationTest, FindsOnlyRegisteredBuffers)
{
    /* A fake address: registration is faked, so nothing reads or writes
     * it, and there's no object there to do pointer arithmetic in */
    constexpr uintptr_t kGpuAddr = 0x5000;
    hipObj::BufferMap   buffers  = makeBuffers();
    void               *gpuBuf   = reinterpret_cast<void *>(kGpuAddr);

    EXPECT_EQ(buffers.find(gpuBuf), nullptr);
    EXPECT_FALSE(buffers.isRegistered(gpuBuf));

    ASSERT_TRUE(buffers.registerBuffer(gpuBuf, 64, reinterpret_cast<struct ibv_pd *>(1)));
    EXPECT_NE(buffers.find(gpuBuf), nullptr);
    EXPECT_TRUE(buffers.isRegistered(gpuBuf));
    /* Only the start of a registration finds it */
    EXPECT_EQ(buffers.find(reinterpret_cast<void *>(kGpuAddr + 1)), nullptr);

    ASSERT_TRUE(buffers.deregisterBuffer(gpuBuf));
    EXPECT_EQ(buffers.find(gpuBuf), nullptr);
    EXPECT_FALSE(buffers.isRegistered(gpuBuf));
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
