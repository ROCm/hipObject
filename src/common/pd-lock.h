/* SPDX-License-Identifier: MIT
 */

#pragma once

namespace hipObj {

/*!
 * RAII guard serializing ibverbs object-lifecycle calls (QP/CQ/MR
 * create/destroy) against a PD shared across threads -- e.g. an
 * RcListener's background accept-loop thread (rc-listener.cpp) and
 * BufferMap's ibv_reg_mr()/ibv_dereg_mr() (buffer.cpp), which can run
 * concurrently against the same PD. libibverbs/mlx5 do not guarantee
 * thread safety for concurrent object creation/destruction sharing one
 * PD (only for posting work requests on already-created, distinct
 * QPs), so every such call outside an accept loop itself (which
 * already serializes its own calls, being single-threaded per
 * listener) must hold this guard for its duration. Never held across a
 * blocking wait. One guard is shared across every PD in the process,
 * since BufferMap has no way to know which listener's PD a given
 * buffer was registered against.
 */
class PdLockGuard {
public:
    PdLockGuard();
    ~PdLockGuard();
    PdLockGuard(const PdLockGuard&) = delete;
    PdLockGuard& operator=(const PdLockGuard&) = delete;
};

} // namespace hipObj
