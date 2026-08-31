/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2-state.h"

namespace hipObj {
namespace v2 {

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle:
      return "Idle";
    case Phase::Negotiating:
      return "Negotiating";
    case Phase::Connecting:
      return "Connecting";
    case Phase::Ready:
      return "Ready";
    case Phase::Transferring:
      return "Transferring";
    case Phase::Draining:
      return "Draining";
  }
  return "?";
}

bool beginNegotiate(Phase& p, bool apply) {
  if (p != Phase::Idle)
    return false;
  if (apply)
    p = Phase::Negotiating;
  return true;
}

bool prepareOk(Phase& p, bool apply) {
  if (p != Phase::Negotiating)
    return false;
  if (apply)
    p = Phase::Connecting;
  return true;
}

bool connectOk(Phase& p, bool apply) {
  if (p != Phase::Connecting)
    return false;
  if (apply)
    p = Phase::Ready;
  return true;
}

bool sendReady(Phase& p, bool apply) {
  if (p != Phase::Ready)
    return false;
  if (apply)
    p = Phase::Transferring;
  return true;
}

bool transferDone(Phase& p, bool apply) {
  if (p != Phase::Transferring)
    return false;
  if (apply)
    p = Phase::Draining;
  return true;
}

bool fail(Phase& p, bool preExpose, bool apply) {
  if (preExpose && p == Phase::Idle)
    return false; /* nothing started */
  if (preExpose) {
    /* Local validation failure before the first callback: back to Idle
     * is only legal from Negotiating before exposure; treat Idle-only
     * returns as no-ops for other phases. */
    if (apply)
      p = Phase::Idle;
    return true;
  }
  if (apply)
    p = Phase::Draining;
  return true;
}

bool drained(Phase& p, bool apply) {
  if (p != Phase::Draining)
    return false;
  if (apply)
    p = Phase::Idle;
  return true;
}

bool exposed(Phase p) {
  return p != Phase::Idle;
}

} // namespace v2
} // namespace hipObj
