/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* hipobj-rc-v2 client state machine.
 *
 * Tracks the per-transfer phases described by the v2 design:
 *
 *   Idle -> Negotiating (PREPARE sent)
 *        -> Connecting  (PREPARE ok, QP transition in progress)
 *        -> Ready       (QP at RTS, receive posted)
 *        -> Transferring (READY sent, waiting for FINAL)
 *        -> Draining    (terminal; quiesce then Idle)
 *
 * Only failures observed before the first callback invocation may return
 * directly to Idle; once any external party (server) could have observed
 * the request, every failure path goes through Draining.
 *
 * The state machine is pure: transitions are validated and applied
 * without touching verbs or HIP, so the whole matrix is unit-testable
 * without hardware.
 */

#pragma once

#include <cstdint>

namespace hipObj {
namespace v2 {

enum class Phase : uint8_t {
  Idle = 0,
  Negotiating,
  Connecting,
  Ready,
  Transferring,
  Draining,
};

const char* phaseName(Phase p);

/* Pure transition checks. Each returns true when the transition is legal
 * from the current phase and applies it when apply is non-null. */
bool beginNegotiate(Phase& p, bool apply = true);
bool prepareOk(Phase& p, bool apply = true);
bool connectOk(Phase& p, bool apply = true);
bool sendReady(Phase& p, bool apply = true);
bool transferDone(Phase& p, bool apply = true);

/* Failure transition: before the first callback (preExpose) the machine
 * returns to Idle; otherwise it must go through Draining. Returns true
 * and applies when legal. */
bool fail(Phase& p, bool preExpose, bool apply = true);

/* Draining completes back to Idle. */
bool drained(Phase& p, bool apply = true);

/* True when the phase has exposed the request externally (callback
 * started). Used to pick the failure target. */
bool exposed(Phase p);

} // namespace v2
} // namespace hipObj
