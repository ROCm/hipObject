/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "abandoned-copies.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include <hip/hip_runtime_api.h>

#include "buffer.h"
#include "hip-seam.h"
#include "ibv-ptr.h"
#include "thread-safety.h"

namespace hipObj {

/* Nothing else can use it while it's destroyed, so this doesn't lock */
AbandonedCopies::~AbandonedCopies()
{
    size_t leaked = 0;
    for (Copy &copy : copies_) {
        if (!isOver(copy)) {
            copy.entry->leakHostBuf = true;
            dropOwnership(copy.done);
            ++leaked;
        }
    }
    if (leaked != 0) {
        fprintf(stderr,
                "hipObj: leaking the staging buffers of the staging copies that timed "
                "out and are still running: %zu.\n",
                leaked);
    }
}

AbandonedCopies::Slot::~Slot()
{
    if (list_ != nullptr) {
        const ScopedLock<Mutex> lock(list_->mutex_);
        --list_->reserved_;
    }
}

AbandonedCopies::Slot
AbandonedCopies::reserve()
{
    const ScopedLock<Mutex> lock(mutex_);
    /* Grows the capacity by at least half, so a list that's growing
     * doesn't reallocate for every slot. Once it's held as many copies
     * and slots as it ever will, it doesn't allocate at all. */
    const size_t needed = copies_.size() + reserved_ + 1;
    if (needed > copies_.capacity()) {
        copies_.reserve(std::max(needed, copies_.capacity() + (copies_.capacity() / 2)));
    }
    ++reserved_;
    return Slot(*this);
}

void
AbandonedCopies::add(Slot &slot, HipEventPtr done, std::shared_ptr<const BufferMap::BufEntry> entry) noexcept
{
    const ScopedLock<Mutex> lock(mutex_);
    /* The capacity is at least the size plus the slots set aside, which
     * include this one, so this doesn't reallocate */
    copies_.push_back(Copy{.entry = std::move(entry), .done = std::move(done)});
    --reserved_;
    slot.list_ = nullptr;
}

void
AbandonedCopies::reclaim()
{
    (void)release(nullptr);
}

bool
AbandonedCopies::running(const BufferMap::BufEntry &entry)
{
    return release(&entry);
}

size_t
AbandonedCopies::size() const
{
    const ScopedLock<Mutex> lock(mutex_);
    return copies_.size();
}

size_t
AbandonedCopies::capacity() const
{
    const ScopedLock<Mutex> lock(mutex_);
    return copies_.capacity();
}

bool
AbandonedCopies::release(const BufferMap::BufEntry *entry)
{
    /* Declared before the lock, so the copies that are over are released
     * after the lock is: their events are destroyed, and the last
     * release of a registration frees its staging buffer */
    std::vector<Copy> over;
    bool              stillRunning = false;

    /* The events are queried with the lock held, so a thread that finds
     * none of an entry's copies running can't miss one that another
     * thread is querying. A query doesn't wait for the copy. */
    const ScopedLock<Mutex> lock(mutex_);
    for (auto it = copies_.begin(); it != copies_.end();) {
        if (entry != nullptr && it->entry.get() != entry) {
            ++it;
        }
        else if (isOver(*it)) {
            over.push_back(std::move(*it));
            it = copies_.erase(it);
        }
        else {
            stillRunning = true;
            ++it;
        }
    }
    return stillRunning;
}

/* An error also means the copy is over: it failed, as
 * stageCopyWithDeadline() assumes when a query fails */
bool
AbandonedCopies::isOver(const Copy &copy) const
{
    return runtime_->eventQuery(copy.done.get()) != hipErrorNotReady;
}

} // namespace hipObj
