/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Owning smart pointers for ibverbs objects.
 *
 * Each deleter releases its object through ibv() and ignores
 * the verb's return code.
 *
 * Never assign nullptr to one of these pointers after destroying the
 * object by hand: the assignment runs the deleter and destroys it a
 * second time. Use dropOwnership() instead. */

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <tuple>

#include "hipobj-span.h"
#include "ibv-core.h"

namespace hipObj {

/* The deleters are defined in ibv-wrapper.cpp so this header doesn't
 * pull in the verbs seam. */
struct IbvContextDeleter {
    void operator()(struct ibv_context *ctx) const noexcept;
};

struct IbvPdDeleter {
    void operator()(struct ibv_pd *pd) const noexcept;
};

struct IbvCqDeleter {
    void operator()(struct ibv_cq *cq) const noexcept;
};

struct IbvQpDeleter {
    void operator()(struct ibv_qp *qp) const noexcept;
};

struct IbvMrDeleter {
    void operator()(struct ibv_mr *mr) const noexcept;
};

struct IbvDeviceListDeleter {
    void operator()(struct ibv_device **list) const noexcept;
};

using IbvContextPtr = std::unique_ptr<struct ibv_context, IbvContextDeleter>;
using IbvPdPtr      = std::unique_ptr<struct ibv_pd, IbvPdDeleter>;
using IbvCqPtr      = std::unique_ptr<struct ibv_cq, IbvCqDeleter>;
using IbvQpPtr      = std::unique_ptr<struct ibv_qp, IbvQpDeleter>;
using IbvMrPtr      = std::unique_ptr<struct ibv_mr, IbvMrDeleter>;

/* The NULL-terminated array returned by ibv_get_device_list() */
using IbvDeviceListPtr = std::unique_ptr<struct ibv_device *[], IbvDeviceListDeleter>;

/* Views the devices in list, given the count ibv_get_device_list()
 * returned with it. Empty when list is null or count isn't positive. */
inline std::span<struct ibv_device *>
deviceSpan(const IbvDeviceListPtr &list, int count) noexcept
{
    if (!list || count <= 0) {
        return {};
    }
    return spanFromPointer(list.get(), static_cast<size_t>(count));
}

/* Gives up ownership without running the deleter, for an object that
 * was already destroyed some other way or is deliberately leaked */
template <typename T, typename D>
void
dropOwnership(std::unique_ptr<T, D> &ptr) noexcept
{
    std::ignore = ptr.release();
}

} // namespace hipObj
