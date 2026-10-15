/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Staging copies that timed out */

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "buffer.h"
#include "hip-seam.h"

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

    /* Takes over a copy that timed out: done is the event that tracks it,
     * and entry is the registration whose staging buffer it uses. If it
     * can't keep track of the copy, it leaks the event and the staging
     * buffer instead. */
    void add(HipEventPtr done, std::shared_ptr<const BufferMap::BufEntry> entry) noexcept;

    /* Releases the copies that are over */
    void reclaim();

    /* Whether a copy that uses entry's staging buffer is still running,
     * after releasing those of its copies that are over */
    [[nodiscard]] bool running(const BufferMap::BufEntry &entry);

    /* The number of copies that haven't been released */
    [[nodiscard]] size_t size() const;

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
    bool release(const BufferMap::BufEntry *entry);

    /* Whether copy has finished or failed. Doesn't wait for it. */
    [[nodiscard]] bool isOver(const Copy &copy) const;

    const HipRuntime *runtime_;
    /* Guards copies_ */
    mutable std::mutex mutex_{};
    std::vector<Copy>  copies_{};
};

} // namespace hipObj
