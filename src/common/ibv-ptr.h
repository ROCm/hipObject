/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Owning smart pointers for ibverbs objects.
 *
 * Each deleter releases its object through the ibv wrapper and ignores
 * the verb's return code, so a deleter is only the last-resort cleanup.
 * Teardown that has to react to a failed destroy (the v2 poison/retry
 * paths) calls ibvDestroy() instead, which gives up ownership only when
 * the verb succeeded.
 *
 * Never assign nullptr to one of these pointers after destroying the
 * object by hand: the assignment runs the deleter and destroys it a
 * second time. Use ibvDestroy() or dropOwnership() instead. */

#pragma once

#include <memory>
#include <tuple>

#include "ibv-core.h"

namespace hipObj {

/* The deleters are defined in ibv-wrapper.cpp so this header doesn't
 * pull in the wrapper. */
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

/* Destroys the object and gives up ownership only when the verb
 * succeeded. On failure the pointer still owns the object so a retry
 * can find it. Returns the verb's result (0 for an empty pointer). */
int ibvDestroy(IbvQpPtr &qp);
int ibvDestroy(IbvCqPtr &cq);

/* Gives up ownership without running the deleter, for an object that
 * was already destroyed some other way or is deliberately leaked */
template <typename T, typename D>
void
dropOwnership(std::unique_ptr<T, D> &ptr) noexcept
{
    std::ignore = ptr.release();
}

} // namespace hipObj
