/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Owning smart pointers for ibverbs objects, and the helpers for them
 * and the other owning pointers whose deleters hold the interface they
 * release through (HostBufPtr and HipEventPtr).
 *
 * Each deleter holds the Verbs the object came from, releases the
 * object through it, and ignores the verb's return code. The Verbs has
 * to outlive the pointer, and everything its ownership moves to. A
 * deleter only reads its own pointer, so pointers can be destroyed on
 * different threads whenever their Verbs can be called from them.
 *
 * A default-constructed deleter has no Verbs, so a default-constructed
 * pointer can hold nothing. Create a pointer that owns an object with
 * own(), never with Ptr(raw) or ptr.reset(raw), which would keep a
 * deleter with no Verbs to release the object through.
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

class Verbs;

/* The deleters are defined in ibv-wrapper.cpp so this header doesn't
 * pull in the verbs seam. */
struct IbvContextDeleter {
    const Verbs *verbs = nullptr;
    void         operator()(struct ibv_context *ctx) const noexcept;
};

struct IbvPdDeleter {
    const Verbs *verbs = nullptr;
    void         operator()(struct ibv_pd *pd) const noexcept;
};

struct IbvCqDeleter {
    const Verbs *verbs = nullptr;
    void         operator()(struct ibv_cq *cq) const noexcept;
};

struct IbvQpDeleter {
    const Verbs *verbs = nullptr;
    void         operator()(struct ibv_qp *qp) const noexcept;
};

struct IbvMrDeleter {
    const Verbs *verbs = nullptr;
    void         operator()(struct ibv_mr *mr) const noexcept;
};

struct IbvDeviceListDeleter {
    const Verbs *verbs = nullptr;
    void         operator()(struct ibv_device **list) const noexcept;
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

/* A Ptr that owns raw, which may be null, and releases it through
 * dependency, the Verbs or HipRuntime it came from:
 * own<IbvQpPtr>(verbs, verbs.create_qp(...)). dependency has to outlive
 * the pointer. */
template <typename Ptr, typename Dependency>
Ptr
own(const Dependency &dependency, typename Ptr::pointer raw) noexcept
{
    return Ptr(raw, typename Ptr::deleter_type{&dependency});
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
