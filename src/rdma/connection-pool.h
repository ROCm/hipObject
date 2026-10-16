/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The connections that transfers use */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "thread-safety.h"
#include "transport.h"

namespace hipObj {

struct RdmaToken;

/* A fixed set of connections on one device, for transfers to lease one at
 * a time each. Each connection has its own CQ, so a transfer only sees its
 * own completions, and its own QP, which the transfer can connect to the
 * peer the server names. A connection that was connected is reset to INIT
 * when its lease ends, so the next transfer can connect it to another
 * peer.
 *
 * A connection that can't be reset is replaced with a new one, and one
 * that can't be replaced is dropped, so the pool can shrink. Several
 * threads can use it at once. */
class ConnectionPool {
public:
    /* What each connection's CQ holds, and its QP's work requests. A
     * transfer posts no work of its own: the peer reads or writes the
     * buffer. */
    static constexpr int      kCqSize    = 16;
    static constexpr uint32_t kMaxSendWr = 8;
    static constexpr uint32_t kMaxRecvWr = 8;

    /* Creates count connections on device, and keeps device alive for
     * them. It stops at the first connection it can't create, so size()
     * reports how many it created. */
    ConnectionPool(std::shared_ptr<const RdmaDevice> device, size_t count);
    ~ConnectionPool() = default;

    /* Lives in one place, and isn't copied or moved */
    ConnectionPool(const ConnectionPool &)            = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;
    ConnectionPool(ConnectionPool &&)                 = delete;
    ConnectionPool &operator=(ConnectionPool &&)      = delete;

    /* One connection, for one transfer, from acquire(). It returns the
     * connection to the pool when it's destroyed, after resetting it if
     * connect() was called. The pool has to outlive it, and it has to be
     * destroyed on the thread that acquired it, which it counts as holding
     * a lease until then (see acquire()). */
    class Lease final {
    public:
        ~Lease();

        /* Moving hands the connection over, and leaves other with none to
         * return */
        Lease(Lease &&other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_), connected_(other.connected_)
        {
        }

        Lease(const Lease &)            = delete;
        Lease &operator=(const Lease &) = delete;
        Lease &operator=(Lease &&)      = delete;

        /* The connection, with its QP in INIT until connect() */
        [[nodiscard]] RcConnection &connection() noexcept;

        /* connectRcPeer() on the connection. Whether or not it succeeds,
         * the connection is reset when the lease ends, since a transition
         * that failed may have followed one that didn't. */
        [[nodiscard]] bool connect(const RdmaToken &peerToken);

    private:
        friend class ConnectionPool;

        Lease(ConnectionPool &pool, size_t index) noexcept;

        /* Null once it's been moved from */
        ConnectionPool *pool_;
        /* The connection's index in the pool's slots_ */
        size_t index_;
        bool   connected_ = false;
    };

    /* Why acquire() returned no lease: the pool was closed, it has no
     * connections left, or they're all leased and the calling thread
     * can't wait for one */
    enum class AcquireError { Closed, NoConnections, Busy };

    /* A lease on a connection that no other lease holds, waiting for one
     * to be returned if they're all leased. A thread that already holds a
     * lease, from any pool, gets Busy instead of waiting: the lease it
     * would wait for could be its own, as when a transfer's callback
     * starts another transfer. */
    [[nodiscard]] std::variant<Lease, AcquireError> acquire() HIPOBJ_REQUIRES(!mutex_);

    /* Makes acquire() fail with Closed from now on, including the calls
     * that are waiting. Leases already held aren't affected. */
    void close() HIPOBJ_REQUIRES(!mutex_);

    /* The number of connections, leased or not */
    [[nodiscard]] size_t size() const HIPOBJ_REQUIRES(!mutex_);

    /* The number of connections that aren't leased */
    [[nodiscard]] size_t available() const HIPOBJ_REQUIRES(!mutex_);

private:
    /* Takes back the connection at index, from a lease that's ending. If
     * it was connected, it's reset, or replaced if it can't be, without
     * holding mutex_; if it can't be replaced either, it's dropped. */
    void release(size_t index, bool connected) noexcept HIPOBJ_REQUIRES(!mutex_);

    /* Resets conn, or replaces it with a new connection. False, with conn
     * empty, if neither works. */
    [[nodiscard]] bool restore(std::optional<RcConnection> &conn) const noexcept;

    std::shared_ptr<const RdmaDevice> device_;
    /* One per connection the pool created. Its size never changes, so the
     * slots stay where they are. A slot is only used by the lease that
     * holds its index, or by release() for that lease, so it needs no
     * lock; a dropped slot is empty, and no lease gets it again. */
    std::vector<std::optional<RcConnection>> slots_{};
    /* Guards free_, live_, and closed_ */
    mutable Mutex mutex_{};
    /* Signalled when a connection is returned, or when acquire() has
     * another reason to stop waiting */
    ConditionVariable returned_{};
    /* The indexes of the connections that aren't leased. Its capacity is
     * the number of slots, so returning a connection doesn't allocate. */
    std::vector<size_t> free_ HIPOBJ_GUARDED_BY(mutex_){};
    /* The number of slots that haven't been dropped */
    size_t live_ HIPOBJ_GUARDED_BY(mutex_)   = 0;
    bool   closed_ HIPOBJ_GUARDED_BY(mutex_) = false;
};

} // namespace hipObj
