/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "connection-pool.h"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

#include "transport.h"

namespace hipObj {

namespace {

    /* The number of leases the calling thread holds, from any pool. Each
     * thread has its own, so it needs no lock. */
    thread_local size_t t_leasesHeld = 0;

} // namespace

ConnectionPool::ConnectionPool(std::shared_ptr<const RdmaDevice> device, size_t count)
    : device_(std::move(device))
{
    slots_.reserve(count);
    free_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::optional<RcConnection> conn = createRcConnection(*device_, kCqSize, kMaxSendWr, kMaxRecvWr);
        if (!conn) {
            break;
        }
        slots_.push_back(std::move(conn));
        free_.push_back(i);
    }
    live_ = slots_.size();
}

ConnectionPool::Lease::Lease(ConnectionPool &pool, size_t index) noexcept : pool_(&pool), index_(index)
{
    ++t_leasesHeld;
}

/* A lease that's been moved from was never counted by the move, so it
 * isn't uncounted either */
ConnectionPool::Lease::~Lease()
{
    if (pool_ != nullptr) {
        pool_->release(index_, connected_);
        --t_leasesHeld;
    }
}

RcConnection &
ConnectionPool::Lease::connection() noexcept
{
    return *pool_->slots_[index_];
}

bool
ConnectionPool::Lease::connect(const RdmaToken &peerToken)
{
    connected_ = true;
    return connectRcPeer(connection(), peerToken);
}

std::variant<ConnectionPool::Lease, ConnectionPool::AcquireError>
ConnectionPool::acquire()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (t_leasesHeld == 0) {
        returned_.wait(lock, [this] { return closed_ || !free_.empty() || live_ == 0; });
    }
    if (closed_) {
        return AcquireError::Closed;
    }
    if (free_.empty()) {
        return live_ == 0 ? AcquireError::NoConnections : AcquireError::Busy;
    }
    const size_t index = free_.back();
    free_.pop_back();
    return Lease(*this, index);
}

void
ConnectionPool::close()
{
    {
        const std::scoped_lock<std::mutex> lock(mutex_);
        closed_ = true;
    }
    returned_.notify_all();
}

size_t
ConnectionPool::size() const
{
    const std::scoped_lock<std::mutex> lock(mutex_);
    return live_;
}

size_t
ConnectionPool::available() const
{
    const std::scoped_lock<std::mutex> lock(mutex_);
    return free_.size();
}

bool
ConnectionPool::restore(std::optional<RcConnection> &conn) const noexcept
{
    try {
        if (resetRcConnection(*conn)) {
            return true;
        }
        /* The old QP and CQ are destroyed before the new ones are created */
        conn.reset();
        std::optional<RcConnection> fresh = createRcConnection(*device_, kCqSize, kMaxSendWr, kMaxRecvWr);
        if (fresh) {
            conn.emplace(std::move(*fresh));
            return true;
        }
    }
    catch (...) {
        /* Treated as a failure, since a lease that's ending can't report
         * an error */
    }
    conn.reset();
    return false;
}

void
ConnectionPool::release(size_t index, bool connected) noexcept
{
    const bool usable = !connected || restore(slots_[index]);

    size_t left = 0;
    {
        const std::scoped_lock<std::mutex> lock(mutex_);
        if (usable) {
            /* The capacity is the number of slots, so this doesn't
             * reallocate */
            free_.push_back(index);
        }
        else {
            --live_;
        }
        left = live_;
    }

    if (usable) {
        returned_.notify_one();
        return;
    }
    /* Waiters stop waiting once there's nothing left to wait for */
    if (left == 0) {
        returned_.notify_all();
    }
    fprintf(stderr,
            "hipObj: could neither reset nor replace an RDMA connection after a transfer, so it's "
            "dropped; %zu of %zu are left.\n",
            left, slots_.size());
}

} // namespace hipObj
