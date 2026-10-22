/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "abandoned-copies.h"
#include "buffer.h"
#include "fake-device.h"
#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj-private.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-ptr.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using RegisterResult   = hipObj::BufferMap::RegisterResult;
using DeregisterResult = hipObj::BufferMap::DeregisterResult;

constexpr int kThreads    = 8;
constexpr int kIterations = 50;

ibv_pd *
fakePd()
{
    return hipObj::addressToPointer<ibv_pd *>(1);
}

/* Fake address number index, for registrations that are faked, so
 * nothing reads or writes it */
void *
fakeAddress(size_t index)
{
    return hipObj::addressToPointer<void *>(0x100000 + (index * 0x1000));
}

/* Runs body(index) on count threads, released together so their calls
 * overlap as much as possible, and returns when all of them are done */
void
runThreads(int count, const std::function<void(int)> &body)
{
    std::latch                start(count);
    std::vector<std::jthread> threads;
    threads.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&start, &body, i] {
            start.arrive_and_wait();
            body(i);
        });
    }
    // Each std::jthread joins its thread when it's destroyed
}

/* Fake verbs that, once a hook is set, call it from the next reg_mr()
 * before registering, as though another thread ran it then. Only set a
 * hook while one thread uses it. */
class RacingVerbs final : public hipObjTest::FakeVerbs {
public:
    using FakeVerbs::FakeVerbs;

    [[nodiscard]] ibv_mr *reg_mr(ibv_pd *pd, void *addr, size_t length, int access) const override
    {
        if (onRegister_) {
            /* Cleared first, so registering from the hook doesn't call it
             * again */
            const std::function<void()> hook = std::exchange(onRegister_, nullptr);
            hook();
        }
        return FakeVerbs::reg_mr(pd, addr, length, access);
    }

    void setOnRegister(std::function<void()> hook)
    {
        onRegister_ = std::move(hook);
    }

private:
    /* reg_mr() is const, and clears it */
    mutable std::function<void()> onRegister_{};
};

/* BufferMap with the fake verbs and HIP runtime */
class BufferRegistrationTest : public ::testing::Test {
protected:
    [[nodiscard]] hipObj::BufferMap makeBuffers() const
    {
        return {verbs_, hip_, {}, false};
    }

    hipObjTest::FakeDevice hipDevice_{};
    RacingVerbs            verbs_{hipDevice_};
    hipObjTest::FakeHip    hip_{hipDevice_};
};

TEST_F(BufferRegistrationTest, HostRegistrationUsesCallerMemory)
{
    hipObj::BufferMap    buffers = makeBuffers();
    std::array<char, 32> hostBuf{};

    EXPECT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), fakePd()),
              RegisterResult::Registered);
    auto entry = buffers.find(hostBuf.data());
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->mr, nullptr);
    EXPECT_EQ(entry->mr->addr, hostBuf.data());
    EXPECT_EQ(entry->hostBuf, nullptr);
    EXPECT_EQ(entry->remoteAddr, hipObj::pointerToAddress(hostBuf.data()));
    EXPECT_EQ(entry->size, hostBuf.size());
    EXPECT_FALSE(entry->isDmabuf);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 0);

    entry.reset();
    EXPECT_EQ(buffers.deregisterBuffer(hostBuf.data()), DeregisterResult::Deregistered);
    EXPECT_EQ(hipDevice_.log.hostFree, 0);
}

TEST_F(BufferRegistrationTest, DirectGpuRegistrationRequiresDeviceSync)
{
    hipObj::BufferMap buffers = makeBuffers();
    void             *gpuBuf  = hipObj::addressToPointer<void *>(0x4000);

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::Registered);
    auto entry = buffers.find(gpuBuf);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->isDmabuf);
    EXPECT_EQ(entry->hostBuf, nullptr);
    EXPECT_EQ(entry->remoteAddr, hipObj::pointerToAddress(gpuBuf));

    entry.reset();
    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::Deregistered);
}

TEST_F(BufferRegistrationTest, GpuRegistrationFallbackOwnsAllocatedHostBuffer)
{
    hipObj::BufferMap buffers    = makeBuffers();
    void             *gpuBuf     = hipObj::addressToPointer<void *>(0x1000);
    hipDevice_.fail.registerAddr = gpuBuf;

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::Registered);
    auto entry = buffers.find(gpuBuf);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->mr, nullptr);
    EXPECT_EQ(entry->mr->addr, hipDevice_.lastHostMalloc);
    EXPECT_EQ(entry->hostBuf.get(), hipDevice_.lastHostMalloc);
    EXPECT_EQ(entry->remoteAddr, hipObj::pointerToAddress(hipDevice_.lastHostMalloc.load()));
    EXPECT_FALSE(entry->isDmabuf);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 1);

    entry.reset();
    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::Deregistered);
    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_EQ(hipDevice_.lastHostFree, hipDevice_.lastHostMalloc);
}

/* Destroying the map, as destroying the library's instance does,
 * releases what it holds */
TEST_F(BufferRegistrationTest, DestroyingFreesOwnedFallbackHostBuffers)
{
    {
        hipObj::BufferMap buffers    = makeBuffers();
        void             *gpuBuf     = hipObj::addressToPointer<void *>(0x2000);
        hipDevice_.fail.registerAddr = gpuBuf;

        EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::Registered);
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
    hipObj::BufferMap    buffers = makeBuffers();
    auto                *pd      = fakePd();
    void                *gpuBuf  = hipObj::addressToPointer<void *>(0x3000);
    std::array<char, 32> hostBuf{};

    EXPECT_EQ(buffers.registerBuffer(gpuBuf, hipObj::MAX_MR_SIZE + 1, pd), RegisterResult::Failed);
    EXPECT_EQ(buffers.registerHostBuffer(hostBuf.data(), hipObj::MAX_MR_SIZE + 1, pd),
              RegisterResult::Failed);
    EXPECT_EQ(hipDevice_.log.registerMr, 0);

    ASSERT_EQ(buffers.registerBuffer(gpuBuf, 64, pd), RegisterResult::Registered);
    ASSERT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), pd), RegisterResult::Registered);
    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, pd), RegisterResult::AlreadyRegistered);
    EXPECT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), pd),
              RegisterResult::AlreadyRegistered);
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
    void               *gpuBuf   = hipObj::addressToPointer<void *>(kGpuAddr);

    EXPECT_EQ(buffers.find(gpuBuf), nullptr);
    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::NotRegistered);

    ASSERT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::Registered);
    EXPECT_NE(buffers.find(gpuBuf), nullptr);
    /* Only the start of a registration finds it */
    EXPECT_EQ(buffers.find(hipObj::addressToPointer<void *>(kGpuAddr + 1)), nullptr);

    ASSERT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::Deregistered);
    EXPECT_EQ(buffers.find(gpuBuf), nullptr);
    EXPECT_EQ(buffers.size(), 0U);
}

/* An entry that find() returned keeps the buffer registered until it's
 * released */
TEST_F(BufferRegistrationTest, DeregisterIsBusyWhileAnEntryIsHeld)
{
    hipObj::BufferMap buffers    = makeBuffers();
    void             *gpuBuf     = hipObj::addressToPointer<void *>(0x6000);
    hipDevice_.fail.registerAddr = gpuBuf;
    ASSERT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::Registered);

    auto held = buffers.find(gpuBuf);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::Busy);
    EXPECT_EQ(buffers.find(gpuBuf), held);
    EXPECT_EQ(hipDevice_.log.deregMr, 0);
    EXPECT_EQ(hipDevice_.log.hostFree, 0);

    held.reset();
    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::Deregistered);
    EXPECT_EQ(hipDevice_.log.deregMr, 1);
    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_EQ(buffers.deregisterBuffer(gpuBuf), DeregisterResult::NotRegistered);
}

/* When another registration of the same address finishes while this one
 * registers, this one loses, and releases its registration */
TEST_F(BufferRegistrationTest, RegistrationThatLosesARaceIsReleased)
{
    hipObj::BufferMap    buffers = makeBuffers();
    std::array<char, 32> hostBuf{};
    const ibv_mr        *winner = nullptr;

    verbs_.setOnRegister([&] {
        EXPECT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), fakePd()),
                  RegisterResult::Registered);
        winner = buffers.find(hostBuf.data())->mr.get();
    });
    EXPECT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), fakePd()),
              RegisterResult::AlreadyRegistered);

    EXPECT_EQ(hipDevice_.log.registerMr, 2);
    EXPECT_EQ(hipDevice_.log.deregMr, 1);
    EXPECT_EQ(buffers.size(), 1U);
    ASSERT_NE(buffers.find(hostBuf.data()), nullptr);
    EXPECT_EQ(buffers.find(hostBuf.data())->mr.get(), winner);
}

/* The same, for a registration through a staging buffer: the loser's MR
 * is deregistered, and its staging buffer freed */
TEST_F(BufferRegistrationTest, StagedRegistrationThatLosesARaceIsReleased)
{
    hipObj::BufferMap buffers    = makeBuffers();
    void             *gpuBuf     = hipObj::addressToPointer<void *>(0x7000);
    hipDevice_.fail.registerAddr = gpuBuf;
    void *winner                 = nullptr;

    verbs_.setOnRegister([&] {
        EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::Registered);
        winner = buffers.find(gpuBuf)->hostBuf.get();
    });
    EXPECT_EQ(buffers.registerBuffer(gpuBuf, 64, fakePd()), RegisterResult::AlreadyRegistered);

    /* Each tried gpuBuf, and then registered a staging buffer */
    EXPECT_EQ(hipDevice_.log.registerMr, 4);
    EXPECT_EQ(hipDevice_.log.hostMalloc, 2);
    EXPECT_EQ(hipDevice_.log.deregMr, 1);
    EXPECT_EQ(hipDevice_.log.hostFree, 1);
    EXPECT_NE(hipDevice_.lastHostFree, winner);
    ASSERT_NE(buffers.find(gpuBuf), nullptr);
    EXPECT_EQ(buffers.find(gpuBuf)->hostBuf.get(), winner);
}

TEST_F(BufferRegistrationTest, RegistrationFailsWhenTheMapIsFull)
{
    constexpr size_t  kMax    = hipObj::BufferMap::kMaxEntries;
    hipObj::BufferMap buffers = makeBuffers();
    for (size_t i = 0; i + 1 < kMax; ++i) {
        ASSERT_EQ(buffers.registerHostBuffer(fakeAddress(i), 64, fakePd()), RegisterResult::Registered);
    }

    /* Another registration takes the last place while this one registers */
    verbs_.setOnRegister([&] {
        EXPECT_EQ(buffers.registerHostBuffer(fakeAddress(kMax - 1), 64, fakePd()),
                  RegisterResult::Registered);
    });
    EXPECT_EQ(buffers.registerHostBuffer(fakeAddress(kMax), 64, fakePd()), RegisterResult::Failed);
    EXPECT_EQ(buffers.size(), kMax);
    EXPECT_EQ(hipDevice_.log.deregMr, 1);

    /* A full map refuses before registering */
    const int registered = hipDevice_.log.registerMr;
    EXPECT_EQ(buffers.registerHostBuffer(fakeAddress(kMax + 1), 64, fakePd()), RegisterResult::Failed);
    EXPECT_EQ(buffers.registerBuffer(fakeAddress(kMax + 1), 64, fakePd()), RegisterResult::Failed);
    EXPECT_EQ(hipDevice_.log.registerMr, registered);
}

TEST_F(BufferRegistrationTest, OneOfManyThreadsRegisteringAnAddressWins)
{
    hipObj::BufferMap    buffers = makeBuffers();
    std::array<char, 32> hostBuf{};
    std::atomic<int>     registered{0};

    runThreads(kThreads, [&](int) {
        const RegisterResult result = buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), fakePd());
        if (result == RegisterResult::Registered) {
            registered.fetch_add(1);
        }
        else {
            EXPECT_EQ(result, RegisterResult::AlreadyRegistered);
        }
    });

    EXPECT_EQ(registered.load(), 1);
    EXPECT_EQ(buffers.size(), 1U);
    /* Every registration that lost was released */
    EXPECT_EQ(hipDevice_.log.registerMr - hipDevice_.log.deregMr, 1);
}

/* Threads that look a buffer up while another deregisters and registers
 * it again only ever find it whole, and it's never deregistered while one
 * of them holds it */
TEST_F(BufferRegistrationTest, LookupsDuringDeregistration)
{
    hipObj::BufferMap    buffers = makeBuffers();
    std::array<char, 32> hostBuf{};
    ASSERT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), fakePd()),
              RegisterResult::Registered);

    runThreads(kThreads, [&](int t) {
        for (int i = 0; i < kIterations; ++i) {
            if (t == 0) {
                const DeregisterResult result = buffers.deregisterBuffer(hostBuf.data());
                EXPECT_NE(result, DeregisterResult::NotRegistered);
                if (result == DeregisterResult::Deregistered) {
                    EXPECT_EQ(buffers.registerHostBuffer(hostBuf.data(), hostBuf.size(), fakePd()),
                              RegisterResult::Registered);
                }
                continue;
            }
            if (const auto entry = buffers.find(hostBuf.data())) {
                EXPECT_EQ(entry->size, hostBuf.size());
                EXPECT_EQ(entry->mr->addr, hostBuf.data());
            }
        }
    });

    EXPECT_EQ(buffers.size(), 1U);
    EXPECT_EQ(hipDevice_.log.registerMr - hipDevice_.log.deregMr, 1);
}

/* Copies abandoned and checked on from several threads at once, each
 * thread on its own registration, which stays registered until its
 * copies are over */
TEST_F(BufferRegistrationTest, AbandonedCopiesFromManyThreads)
{
    hipObj::BufferMap       buffers = makeBuffers();
    hipObj::AbandonedCopies abandoned(hip_);
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_EQ(buffers.registerHostBuffer(fakeAddress(static_cast<size_t>(t)), 64, fakePd()),
                  RegisterResult::Registered);
    }
    hipDevice_.eventNeverCompletes = true;

    runThreads(kThreads, [&](int t) {
        const auto entry = buffers.find(fakeAddress(static_cast<size_t>(t)));
        ASSERT_NE(entry, nullptr);
        for (int i = 0; i < kIterations; ++i) {
            /* A copy that finished in time gives its room back */
            {
                const hipObj::AbandonedCopies::Slot unused = abandoned.reserve();
            }
            hipObj::AbandonedCopies::Slot slot = abandoned.reserve();
            abandoned.add(slot, hipObj::own<hipObj::HipEventPtr>(hip_, hipObjTest::fakeEvent()), entry);
            EXPECT_TRUE(abandoned.running(*entry));
            abandoned.reclaim();
        }
    });

    constexpr size_t kCopies = static_cast<size_t>(kThreads) * kIterations;
    EXPECT_EQ(abandoned.size(), kCopies);
    EXPECT_EQ(buffers.deregisterBuffer(fakeAddress(0)), DeregisterResult::Busy);
    EXPECT_EQ(hipDevice_.log.eventDestroy, 0);

    hipDevice_.eventNeverCompletes = false;
    abandoned.reclaim();
    EXPECT_EQ(abandoned.size(), 0U);
    EXPECT_EQ(hipDevice_.log.eventDestroy, static_cast<int>(kCopies));
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(buffers.deregisterBuffer(fakeAddress(static_cast<size_t>(t))),
                  DeregisterResult::Deregistered);
    }
}

/* add() takes a copy over in the room reserve() set aside before the
 * copy started, so it never allocates, and so can't fail and leave a
 * running copy untracked */
TEST_F(BufferRegistrationTest, TakingOverACopyDoesntAllocate)
{
    constexpr size_t        kCopies = 5;
    hipObj::BufferMap       buffers = makeBuffers();
    hipObj::AbandonedCopies abandoned(hip_);
    ASSERT_EQ(buffers.registerHostBuffer(fakeAddress(0), 64, fakePd()), RegisterResult::Registered);
    const auto entry = buffers.find(fakeAddress(0));
    ASSERT_NE(entry, nullptr);
    hipDevice_.eventNeverCompletes = true;

    std::vector<hipObj::AbandonedCopies::Slot> slots;
    slots.reserve(kCopies);
    for (size_t i = 0; i < kCopies; ++i) {
        slots.push_back(abandoned.reserve());
    }
    const size_t capacity = abandoned.capacity();
    EXPECT_GE(capacity, kCopies);
    for (auto &slot : slots) {
        abandoned.add(slot, hipObj::own<hipObj::HipEventPtr>(hip_, hipObjTest::fakeEvent()), entry);
        EXPECT_EQ(abandoned.capacity(), capacity);
    }
    EXPECT_EQ(abandoned.size(), kCopies);

    /* A used slot doesn't give its room back, and the next copy's room is
     * set aside before it starts */
    slots.clear();
    {
        const hipObj::AbandonedCopies::Slot next = abandoned.reserve();
        EXPECT_GT(abandoned.capacity(), kCopies);
    }

    hipDevice_.eventNeverCompletes = false;
    abandoned.reclaim();
    EXPECT_EQ(abandoned.size(), 0U);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
