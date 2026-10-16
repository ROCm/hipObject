/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Staging copies that timed out */

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "buffer.h"
#include "hip-seam.h"
#include "thread-safety.h"

namespace hipObj {

/* Staging copies that didn't finish within the staging timeout (see
 * Options::stageTimeout), each with the event that tracks it and the
 * registration whose staging buffer it uses. A copy that times out can
 * still read or write the staging buffer whenever the GPU gets to it, so
 * the buffer can't be freed, or used for another copy or transfer, until
 * the event reports that the copy is over.
 *
 * Holding the registration keeps the staging buffer allocated, and makes
 * BufferMap::deregisterBuffer() report Busy. A copy is released once it's
 * over, so a copy that was only slow costs nothing in the end; only one
 * that never finishes keeps its staging buffer for good. Several threads
 * can use it at once. */
class AbandonedCopies {
public:
    /* Queries and destroys events through runtime, which has to outlive
     * it */
    explicit AbandonedCopies(const HipRuntime &runtime) noexcept : runtime_(&runtime)
    {
    }

    /* Releases the copies that are over, and leaks the staging buffers
     * of those that aren't, since freeing one could let its copy use
     * freed memory */
    ~AbandonedCopies();

    /* Lives in one place, and isn't copied or moved */
    AbandonedCopies(const AbandonedCopies &)            = delete;
    AbandonedCopies &operator=(const AbandonedCopies &) = delete;
    AbandonedCopies(AbandonedCopies &&)                 = delete;
    AbandonedCopies &operator=(AbandonedCopies &&)      = delete;

    /* Room in the list for one copy, set aside by reserve() before the
     * copy starts. It gives the room back when it's destroyed, unless
     * add() used it. The list has to outlive it. */
    class Slot final {
    public:
        ~Slot();

        /* Moving hands the room over, and leaves other with none to give
         * back */
        Slot(Slot &&other) noexcept : list_(std::exchange(other.list_, nullptr))
        {
        }

        Slot(const Slot &)            = delete;
        Slot &operator=(const Slot &) = delete;
        Slot &operator=(Slot &&)      = delete;

    private:
        friend class AbandonedCopies;

        explicit Slot(AbandonedCopies &list) noexcept : list_(&list)
        {
        }

        /* Null once add() has used the room, or it's been moved from */
        AbandonedCopies *list_;
    };

    /* Sets aside room for a copy that's about to start, so that if it
     * times out, add() can take it over without allocating, and so
     * without failing. Can throw std::bad_alloc, before the copy starts. */
    [[nodiscard]] Slot reserve() HIPOBJ_REQUIRES(!mutex_);

    /* Takes over a copy that timed out, in the room slot set aside: done
     * is the event that tracks it, and entry is the registration whose
     * staging buffer it uses. slot has to have come from this list's
     * reserve(), and not been used or moved from. */
    void add(Slot &slot, HipEventPtr done, std::shared_ptr<const BufferMap::BufEntry> entry) noexcept
        HIPOBJ_REQUIRES(!mutex_);

    /* Releases the copies that are over */
    void reclaim() HIPOBJ_REQUIRES(!mutex_);

    /* Whether a copy that uses entry's staging buffer is still running,
     * after releasing those of its copies that are over */
    [[nodiscard]] bool running(const BufferMap::BufEntry &entry) HIPOBJ_REQUIRES(!mutex_);

    /* The number of copies that haven't been released */
    [[nodiscard]] size_t size() const HIPOBJ_REQUIRES(!mutex_);

    /* The number of copies the list can hold without allocating */
    [[nodiscard]] size_t capacity() const HIPOBJ_REQUIRES(!mutex_);

private:
    /* done is declared after entry, so the event is destroyed before the
     * registration is released */
    struct Copy {
        std::shared_ptr<const BufferMap::BufEntry> entry;
        HipEventPtr                                done;
    };

    /* Releases the copies of entry, or of every registration if it's
     * null, that are over. Returns whether any of them are still
     * running. */
    bool release(const BufferMap::BufEntry *entry) HIPOBJ_REQUIRES(!mutex_);

    /* Whether copy has finished or failed. Doesn't wait for it. */
    [[nodiscard]] bool isOver(const Copy &copy) const;

    const HipRuntime *runtime_;
    /* Guards copies_ and reserved_ */
    mutable Mutex mutex_{};
    /* Its capacity is always at least its size plus reserved_ */
    std::vector<Copy> copies_ HIPOBJ_GUARDED_BY(mutex_){};
    /* The number of slots that reserve() has set aside, and that haven't
     * been used or given back */
    size_t reserved_ HIPOBJ_GUARDED_BY(mutex_) = 0;
};

} // namespace hipObj
