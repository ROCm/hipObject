/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Per-token connection registry for the v2 protocol.
 *
 * Ownership model: the shared DeviceHandle owns
 * ctx/pd plus topology; each RcConnV2 owns exactly one qp/cq pair.
 * Every caller holds the library-wide API lock (ApiGuard, in state.h),
 * so entries, capacity accounting, and the retired ring are consistent
 * without additional locks.
 *
 * Entry lifecycle:
 *
 *   Live --claimDestroy--> Destroying --commitDestroy(remains)-->
 *   Poisoned --claimDestroy(retry)--> Destroying --commitDestroy(all
 *   gone)--> Destroyed --eraseDestroyed--> (removed)
 *
 * abortClaim() cancels a claim before any destroy verb ran; it is a
 * TU-private helper called from exactly one place in
 * releaseConnection().
 *
 * Retired ring: guards against (qpn, psn) reuse by stale peers. A
 * reservation (Reserved, no expiry) is bound to the entry that will
 * need to record its QP; recording flips it to Recorded with a 60 s
 * expiry. Invariant: entry.reservationId != 0 implies entry.qp !=
 * nullptr (one-directional; Busy-deferred entries legally hold a
 * live QP with no reservation).
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "ibv-ptr.h"
#include "replace-by-move.h"
#include "v2-clock.h"

namespace hipObj {

struct DeviceHandle;

/* Owns one qp/cq pair. qp is declared last so it is destroyed before
 * the cq it uses. Move assignment destroys the target first, so it
 * releases them in that order too. */
struct RcConnV2 final {
    IbvCqPtr cq;
    IbvQpPtr qp;
    uint32_t qpNum = 0;

    RcConnV2()                            = default;
    ~RcConnV2()                           = default;
    RcConnV2(const RcConnV2 &)            = delete;
    RcConnV2 &operator=(const RcConnV2 &) = delete;
    RcConnV2(RcConnV2 &&) noexcept        = default;
    RcConnV2 &operator=(RcConnV2 &&other) noexcept
    {
        replaceByMove(*this, std::move(other));
        return *this;
    }
};

namespace v2 {

    /* What releaseConnection() and discardAndRecreateQp() did (mapped to
     * hipObj errors at the API boundary). Each function says what
     * Leftover leaves behind. */
    enum class ReleaseResult {
        Ok,
        Busy,     /* the retired ring is full and nothing changed; retry later */
        Leftover, /* the release or recreate failed */
    };

    using ConnId = uint64_t;

    /* Move-only: the entry owns its connection's qp/cq. device is a
     * non-owning reference to the shared handle. */
    struct ConnectionEntryV2 {
        RcConnV2      conn;
        DeviceHandle *device         = nullptr;
        uint64_t      reservationId  = 0; /* retired-ring slot for this QP */
        uint32_t      clientPsn      = 0; /* client PSN (recorded on destroy) */
        bool          poisoned       = false;
        bool          destroyClaimed = false;
        bool          destroying     = false;
    };

    /* Retired (qpn, psn) reuse guard ring. */
    class RetiredRing {
    public:
        static constexpr size_t               kCapacity = 4096;
        static constexpr std::chrono::seconds kExpiry{60};

        /* Owns one Reserved slot, and unreserves it when it goes out of
         * scope unless record() or release() passed it on. Move-only,
         * like std::unique_ptr, and an empty one owns nothing. The ring
         * must outlive it. */
        class Reservation {
        public:
            Reservation() noexcept = default;

            /* Reserves a slot in ring; empty when the ring is full */
            explicit Reservation(RetiredRing &ring);

            /* Takes ownership of reservationId, already Reserved in ring.
             * 0 gives an empty one. */
            static Reservation adopt(RetiredRing &ring, uint64_t reservationId) noexcept;

            ~Reservation();

            Reservation(const Reservation &)            = delete;
            Reservation &operator=(const Reservation &) = delete;
            Reservation(Reservation &&other) noexcept;
            Reservation &operator=(Reservation &&other) noexcept;

            explicit operator bool() const noexcept
            {
                return id_ != 0;
            }

            /* Records (qpn, psn) in the slot, which then belongs to the
             * ring until it expires. Does nothing when empty. */
            void record(uint32_t qpn, uint32_t psn);

            /* Gives up ownership without unreserving, and returns the
             * reservation id (0 when empty) */
            [[nodiscard]] uint64_t release() noexcept;

        private:
            Reservation(RetiredRing *ring, uint64_t reservationId) noexcept;

            /* Unreserves the slot, if any, and leaves this empty */
            void reset() noexcept;

            RetiredRing *ring_ = nullptr;
            uint64_t     id_   = 0;
        };

        /* Collects expired Recorded slots; returns slots collected. */
        size_t collectExpired(TimePoint now);

        /* Reserves a slot; 0 when the ring is full after collection. */
        uint64_t reserve();

        /* Releases a Reserved reservation (never recorded). */
        void unreserve(uint64_t reservationId) noexcept;

        /* Reserved -> Recorded with (qpn, psn) and a fresh expiry.
         * Precondition: reservationId is Reserved and owned by the caller
         * (guaranteed by construction: all callers hold the API lock and
         * the single-entry ownership invariant). */
        void record(uint64_t reservationId, uint32_t qpn, uint32_t psn);

        bool contains(uint32_t qpn, uint32_t psn) const;

        size_t used() const;
        size_t reservedCount() const;
        size_t recordedCount() const;

    private:
        struct Slot {
            uint64_t  reservationId = 0; /* 0 = free */
            bool      recorded      = false;
            TimePoint expireAt{};
            uint32_t  qpn = 0;
            uint32_t  psn = 0;
        };

        std::vector<Slot> slots_             = std::vector<Slot>(kCapacity);
        uint64_t          nextReservationId_ = 1;
        size_t            used_              = 0;
        size_t            reserved_          = 0;
    };

    class ConnectionRegistry {
    public:
        static constexpr size_t kMaxConnections = 64;

        /* Capacity reservation; pairs with insert()/unreserveSlot(). */
        bool reserveSlot();
        void unreserveSlot();

        /* Inserts an entry (consumes one pending reserve). 0 when the
         * entry would exceed the capacity. */
        ConnId insert(ConnectionEntryV2 &&entry);

        /* Lookup-only visitor; must not call registry mutations or public
         * callbacks from fn. */
        template <typename F> bool withEntry(ConnId id, F &&fn)
        {
            auto it = entries_.find(id);
            if (it == entries_.end()) {
                return false;
            }
            fn(it->second);
            return true;
        }

        /* Live|Poisoned -> Destroying; single claimant. */
        bool claimDestroy(ConnId id);

        /* Destroying -> Poisoned|Destroyed, reflecting destroyed objects. */
        void commitDestroy(ConnId id, bool qpGone, bool cqGone);

        /* Destroyed -> removed. Returns false when the id is unknown or
         * still holds a qp or cq. */
        bool eraseDestroyed(ConnId id);

        bool   isPoisoned(ConnId id) const;
        size_t size() const;
        size_t pendingReserves() const;

        /* Snapshot iteration: fn receives every live id. Mutations from
         * fn are forbidden (lookup-only contract). */
        template <typename F> void forEachId(F &&fn) const
        {
            for (const auto &[id, entry] : entries_) {
                fn(id);
            }
        }

        /* Access to the shared retired ring (API lock held by callers). */
        RetiredRing &retired();

#ifdef HIPOBJ_UNIT_TESTS
        /* Test-only direct insertion bypassing reserveSlot() and
         * retiredReserve(); used to construct states the normal
         * sequencing cannot reach (for example a live qp without a
         * retired-ring reservation, to exercise the defensive busy
         * path). */
        ConnId insertRawForTest(ConnectionEntryV2 &&entry);
#endif

    private:
        std::map<ConnId, ConnectionEntryV2> entries_;
        size_t                              pendingReserves_ = 0;
        RetiredRing                         retired_;
        ConnId                              nextId_ = 1;
    };

    /* Global v2 registry accessor (single instance). */
    ConnectionRegistry &registry();

    /* Test-only registry replacement; returns the previous one. */
    ConnectionRegistry *setRegistryForTest(ConnectionRegistry *r);

} // namespace v2
} // namespace hipObj
