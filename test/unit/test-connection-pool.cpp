/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * The connection pool that transfers lease their connections from:
 * creating the connections, leasing and returning them, resetting,
 * replacing, or dropping a connection that was connected, and waiting for
 * one, against the fake device in fake-device.h.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "connection-pool.h"
#include "fake-device.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "transport.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::ConnectionPool;
using hipObjTest::fake;
using hipObjTest::peerToken;

using Lease        = ConnectionPool::Lease;
using AcquireError = ConnectionPool::AcquireError;

/* How long a test waits to see that a thread is still waiting */
constexpr auto kStillWaiting = std::chrono::milliseconds(50);

/* A lease from pool, or std::nullopt if acquire() fails. Waits if every
 * connection is leased. */
std::optional<Lease>
leaseFrom(ConnectionPool &pool)
{
    auto acquired = pool.acquire();
    if (auto *lease = std::get_if<Lease>(&acquired)) {
        return std::move(*lease);
    }
    return std::nullopt;
}

/* Why acquire() fails, or std::nullopt if it doesn't. Waits if every
 * connection is leased. */
std::optional<AcquireError>
acquireError(ConnectionPool &pool)
{
    const auto acquired = pool.acquire();
    if (const auto *err = std::get_if<AcquireError>(&acquired)) {
        return *err;
    }
    return std::nullopt;
}

/* The fake verbs, with create_qp() throwing once throws is set */
class ThrowingCreateQpVerbs : public hipObjTest::FakeVerbs {
public:
    using FakeVerbs::FakeVerbs;

    ibv_qp *create_qp(ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const override
    {
        if (throws) {
            throw std::runtime_error("thrown by a fake");
        }
        return FakeVerbs::create_qp(pd, qp_init_attr);
    }

    std::atomic<bool> throws{false};
};

class ConnectionPoolTest : public hipObjTest::FakeDeviceTest {
protected:
    void SetUp() override
    {
        FakeDeviceTest::SetUp();
        std::optional<hipObj::RdmaDevice> opened = hipObj::openRdmaDevice(verbs(), topology(), 0);
        ASSERT_TRUE(opened);
        device_ = std::make_shared<const hipObj::RdmaDevice>(std::move(*opened));
    }

    /* A pool of count connections on device_. Forgets the modify_qp()
     * calls that moved their QPs to INIT, so a test sees only the calls it
     * makes. */
    [[nodiscard]] std::unique_ptr<ConnectionPool> makePool(size_t count) const
    {
        auto pool            = std::make_unique<ConnectionPool>(device_, count);
        fake().modifyQpCount = 0;
        return pool;
    }

    /* Every verbs object the fakes created has been destroyed */
    static void expectNothingLeaked()
    {
        const hipObjTest::CallLog &log = fake().log;
        EXPECT_EQ(log.openDevice, log.closeDevice);
        EXPECT_EQ(log.allocPd, log.deallocPd);
        EXPECT_EQ(log.createCq, log.destroyCq);
        EXPECT_EQ(log.createQp, log.destroyQp);
    }

    std::shared_ptr<const hipObj::RdmaDevice> device_{};
};

// ---- Creating the connections ------------------------------------------

TEST_F(ConnectionPoolTest, CreatesTheConnectionsInInit)
{
    const auto pool = makePool(3);
    EXPECT_EQ(pool->size(), 3U);
    EXPECT_EQ(pool->available(), 3U);
    EXPECT_EQ(fake().log.createQp, 3);
    EXPECT_EQ(fake().log.createCq, 3);

    /* Each lease gets a different connection */
    std::vector<Lease> leases;
    std::set<uint32_t> qpNums;
    for (int i = 0; i < 3; ++i) {
        std::optional<Lease> lease = leaseFrom(*pool);
        ASSERT_TRUE(lease);
        const hipObj::RcConnection &conn = lease->connection();
        EXPECT_EQ(&conn.device(), device_.get());
        EXPECT_EQ(conn.qp()->state, IBV_QPS_INIT);
        EXPECT_EQ(conn.cq()->cqe, ConnectionPool::kCqSize);
        qpNums.insert(conn.qp()->qp_num);
        leases.push_back(std::move(*lease));
    }
    EXPECT_EQ(qpNums.size(), 3U);
    EXPECT_EQ(pool->available(), 0U);
    EXPECT_EQ(pool->size(), 3U);

    leases.clear();
    EXPECT_EQ(pool->available(), 3U);
}

TEST_F(ConnectionPoolTest, StopsAtTheFirstConnectionItCantCreate)
{
    fake().fail.qpLimit = 2;
    {
        const auto pool = makePool(4);
        EXPECT_EQ(pool->size(), 2U);
        EXPECT_EQ(pool->available(), 2U);
        EXPECT_EQ(fake().log.createQp, 2);
        /* The CQ created for the third connection is released */
        EXPECT_EQ(fake().log.createCq, 3);
        EXPECT_EQ(fake().log.destroyCq, 1);
    }
    device_.reset();
    expectNothingLeaked();
}

TEST_F(ConnectionPoolTest, KeepsItsDeviceAlive)
{
    auto pool = makePool(2);
    device_.reset();
    EXPECT_EQ(fake().log.closeDevice, 0);

    pool.reset();
    EXPECT_EQ(fake().log.destroyQp, 2);
    EXPECT_EQ(fake().log.destroyCq, 2);
    expectNothingLeaked();
}

TEST_F(ConnectionPoolTest, EmptyPoolHasNoConnections)
{
    const auto pool = makePool(0);
    EXPECT_EQ(pool->size(), 0U);
    EXPECT_EQ(acquireError(*pool), AcquireError::NoConnections);
}

// ---- Leasing and returning ---------------------------------------------

TEST_F(ConnectionPoolTest, ReturnsAnUnconnectedConnectionAsItIs)
{
    const auto pool  = makePool(1);
    uint32_t   qpNum = 0;
    {
        std::optional<Lease> lease = leaseFrom(*pool);
        ASSERT_TRUE(lease);
        qpNum = lease->connection().qp()->qp_num;
        EXPECT_EQ(pool->available(), 0U);
    }
    EXPECT_EQ(pool->available(), 1U);
    EXPECT_EQ(fake().modifyQpCount, 0U);
    EXPECT_EQ(fake().log.pollCq, 0);

    std::optional<Lease> lease = leaseFrom(*pool);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->connection().qp()->qp_num, qpNum);
}

TEST_F(ConnectionPoolTest, MovingALeaseHandsOverTheConnection)
{
    const auto           pool  = makePool(1);
    std::optional<Lease> first = leaseFrom(*pool);
    ASSERT_TRUE(first);
    ibv_qp *const qp = first->connection().qp();

    std::optional<Lease> second;
    second.emplace(std::move(*first));
    first.reset();
    EXPECT_EQ(pool->available(), 0U);
    EXPECT_EQ(second->connection().qp(), qp);

    second.reset();
    EXPECT_EQ(pool->available(), 1U);
}

TEST_F(ConnectionPoolTest, ResetsAConnectedConnectionWhenItsReturned)
{
    const auto pool = makePool(1);
    {
        std::optional<Lease> lease = leaseFrom(*pool);
        ASSERT_TRUE(lease);
        ASSERT_TRUE(lease->connect(peerToken()));
        EXPECT_EQ(lease->connection().qp()->state, IBV_QPS_RTS);
    }
    EXPECT_EQ(pool->available(), 1U);
    EXPECT_GT(fake().log.pollCq, 0);

    /* It can connect again */
    std::optional<Lease> lease = leaseFrom(*pool);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->connection().qp()->state, IBV_QPS_INIT);
    EXPECT_TRUE(lease->connect(peerToken()));
}

TEST_F(ConnectionPoolTest, ResetsAConnectionThatFailedToConnect)
{
    const auto pool = makePool(1);
    {
        std::optional<Lease> lease = leaseFrom(*pool);
        ASSERT_TRUE(lease);
        fake().fail.modifyQpToState = IBV_QPS_RTS;
        EXPECT_FALSE(lease->connect(peerToken()));
        EXPECT_EQ(lease->connection().qp()->state, IBV_QPS_RTR);
        fake().fail.modifyQpToState = -1;
    }
    std::optional<Lease> lease = leaseFrom(*pool);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->connection().qp()->state, IBV_QPS_INIT);
}

TEST_F(ConnectionPoolTest, ReplacesAConnectionItCantReset)
{
    const auto pool  = makePool(2);
    uint32_t   qpNum = 0;
    {
        std::optional<Lease> lease = leaseFrom(*pool);
        ASSERT_TRUE(lease);
        qpNum = lease->connection().qp()->qp_num;
        ASSERT_TRUE(lease->connect(peerToken()));
        fake().fail.modifyQpToState = IBV_QPS_RESET;
    }
    fake().fail.modifyQpToState = -1;
    EXPECT_EQ(fake().log.destroyQp, 1);
    EXPECT_EQ(fake().log.destroyCq, 1);
    EXPECT_EQ(fake().log.createQp, 3);
    EXPECT_EQ(pool->size(), 2U);
    EXPECT_EQ(pool->available(), 2U);

    /* The replacement is the connection returned last, so it's leased
     * first */
    std::optional<Lease> lease = leaseFrom(*pool);
    ASSERT_TRUE(lease);
    EXPECT_NE(lease->connection().qp()->qp_num, qpNum);
    EXPECT_EQ(lease->connection().qp()->state, IBV_QPS_INIT);
    EXPECT_TRUE(lease->connect(peerToken()));
}

TEST_F(ConnectionPoolTest, DropsAConnectionItCantReplace)
{
    {
        const auto pool = makePool(2);
        {
            std::optional<Lease> lease = leaseFrom(*pool);
            ASSERT_TRUE(lease);
            ASSERT_TRUE(lease->connect(peerToken()));
            fake().fail.modifyQpToState = IBV_QPS_RESET;
            fake().fail.createQp        = true;
        }
        EXPECT_EQ(pool->size(), 1U);
        EXPECT_EQ(pool->available(), 1U);
        EXPECT_EQ(fake().log.destroyQp, 1);

        /* The other connection still works */
        const std::optional<Lease> lease = leaseFrom(*pool);
        ASSERT_TRUE(lease);
        EXPECT_EQ(pool->available(), 0U);
    }
    device_.reset();
    expectNothingLeaked();
}

TEST_F(ConnectionPoolTest, DropsAConnectionWhoseReplacementThrows)
{
    ThrowingCreateQpVerbs             throwingVerbs(fake());
    std::optional<hipObj::RdmaDevice> opened = hipObj::openRdmaDevice(throwingVerbs, topology(), 0);
    ASSERT_TRUE(opened);
    ConnectionPool pool(std::make_shared<const hipObj::RdmaDevice>(std::move(*opened)), 1);
    {
        std::optional<Lease> lease = leaseFrom(pool);
        ASSERT_TRUE(lease);
        ASSERT_TRUE(lease->connect(peerToken()));
        fake().fail.modifyQpToState = IBV_QPS_RESET;
        throwingVerbs.throws        = true;
    }
    EXPECT_EQ(pool.size(), 0U);
    EXPECT_EQ(acquireError(pool), AcquireError::NoConnections);
}

// ---- Waiting for a connection ------------------------------------------

TEST_F(ConnectionPoolTest, AcquireWaitsForAConnectionToBeReturned)
{
    const auto           pool = makePool(1);
    std::optional<Lease> held = leaseFrom(*pool);
    ASSERT_TRUE(held);

    std::atomic<bool> leased{false};
    std::jthread      waiter([&] {
        const std::optional<Lease> lease = leaseFrom(*pool);
        EXPECT_TRUE(lease);
        leased = true;
    });
    std::this_thread::sleep_for(kStillWaiting);
    EXPECT_FALSE(leased);

    held.reset();
    waiter.join();
    EXPECT_TRUE(leased);
    EXPECT_EQ(pool->available(), 1U);
}

TEST_F(ConnectionPoolTest, CloseStopsAcquireWaiting)
{
    const auto           pool = makePool(1);
    std::optional<Lease> held = leaseFrom(*pool);
    ASSERT_TRUE(held);

    std::optional<AcquireError> err;
    std::jthread                waiter([&] { err = acquireError(*pool); });
    std::this_thread::sleep_for(kStillWaiting);
    pool->close();
    waiter.join();
    EXPECT_EQ(err, AcquireError::Closed);

    /* A lease already held still returns its connection, but no more are
     * leased */
    held.reset();
    EXPECT_EQ(pool->available(), 1U);
    EXPECT_EQ(acquireError(*pool), AcquireError::Closed);
}

TEST_F(ConnectionPoolTest, DroppingTheLastConnectionStopsAcquireWaiting)
{
    const auto           pool = makePool(1);
    std::optional<Lease> held = leaseFrom(*pool);
    ASSERT_TRUE(held);
    ASSERT_TRUE(held->connect(peerToken()));

    std::optional<AcquireError> err;
    std::jthread                waiter([&] { err = acquireError(*pool); });
    std::this_thread::sleep_for(kStillWaiting);
    fake().fail.modifyQpToState = IBV_QPS_RESET;
    fake().fail.createQp        = true;
    held.reset();
    waiter.join();
    EXPECT_EQ(err, AcquireError::NoConnections);
}

// ---- A thread that holds a lease ---------------------------------------

/* A thread that holds a lease gets Busy rather than waiting, since the
 * lease it would wait for could be its own, and a lease that's moved is
 * still held */
TEST_F(ConnectionPoolTest, ThreadHoldingALeaseDoesntWait)
{
    const auto           pool  = makePool(2);
    std::optional<Lease> first = leaseFrom(*pool);
    ASSERT_TRUE(first);
    /* One is free, so it's leased */
    const std::optional<Lease> second = leaseFrom(*pool);
    ASSERT_TRUE(second);
    EXPECT_EQ(acquireError(*pool), AcquireError::Busy);

    std::optional<Lease> moved;
    moved.emplace(std::move(*first));
    first.reset();
    EXPECT_EQ(acquireError(*pool), AcquireError::Busy);

    /* Closing takes precedence */
    pool->close();
    EXPECT_EQ(acquireError(*pool), AcquireError::Closed);
}

/* A lease from another pool counts too: threads that each held a lease
 * and waited for a connection in another pool could wait for each other */
TEST_F(ConnectionPoolTest, LeaseFromAnotherPoolCounts)
{
    const auto                 pool  = makePool(1);
    const auto                 other = makePool(1);
    const std::optional<Lease> held  = leaseFrom(*pool);
    ASSERT_TRUE(held);

    std::optional<AcquireError> err;
    std::jthread                thread([&] {
        const std::optional<Lease> otherLease = leaseFrom(*other);
        ASSERT_TRUE(otherLease);
        err = acquireError(*pool);
    });
    thread.join();
    EXPECT_EQ(err, AcquireError::Busy);
}

/* A thread that has returned every lease it held, including one it moved,
 * waits again */
TEST_F(ConnectionPoolTest, ThreadThatReturnedItsLeasesWaits)
{
    const auto pool = makePool(1);
    {
        std::optional<Lease> first = leaseFrom(*pool);
        ASSERT_TRUE(first);
        std::optional<Lease> moved;
        moved.emplace(std::move(*first));
    }

    /* Another thread holds the only connection until it's told to return
     * it, after this thread has started waiting */
    std::latch         leased(1);
    std::atomic<bool>  giveBack{false};
    const std::jthread holder([&] {
        const std::optional<Lease> lease = leaseFrom(*pool);
        leased.count_down();
        while (!giveBack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    leased.wait();
    const std::jthread releaser([&] {
        std::this_thread::sleep_for(kStillWaiting);
        giveBack = true;
    });
    EXPECT_TRUE(leaseFrom(*pool));
}

/* Threads that lease, connect, and return connections at once each get a
 * connection no other thread holds. Does the most good in a
 * ThreadSanitizer build. */
TEST_F(ConnectionPoolTest, LeasesFromManyThreads)
{
    constexpr size_t kConnections = 2;
    constexpr int    kThreads     = 8;
    constexpr int    kIterations  = 50;
    const auto       pool         = makePool(kConnections);
    const uint32_t   firstQpNum   = fake().nextQpNum - static_cast<uint32_t>(kConnections);

    std::array<std::atomic<bool>, kConnections> held{};
    std::atomic<int>                            overlaps{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kIterations; ++i) {
                    std::optional<Lease> lease = leaseFrom(*pool);
                    ASSERT_TRUE(lease);
                    const size_t index = lease->connection().qp()->qp_num - firstQpNum;
                    ASSERT_LT(index, kConnections);
                    if (held.at(index).exchange(true)) {
                        ++overlaps;
                    }
                    EXPECT_TRUE(lease->connect(peerToken()));
                    held.at(index) = false;
                }
            });
        }
    }
    EXPECT_EQ(overlaps, 0);
    EXPECT_EQ(pool->available(), kConnections);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
