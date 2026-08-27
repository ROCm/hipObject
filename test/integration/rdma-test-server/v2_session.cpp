/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_session.h"

#include <utility>

namespace hipObj {
namespace v2 {

namespace {

void notifyAll(std::condition_variable& cv) {
  cv.notify_all();
}

} // namespace

bool SessionTable::insert(V2Session&& session) {
  std::lock_guard<std::mutex> guard(mtx_);
  session.ioActive = 1; /* published together with the entry */
  auto [it, ok] = entries_.emplace(session.id, std::move(session));
  if (ok) {
    notifyAll(cv_);
  }
  return ok;
}

bool SessionTable::beginPublishing(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.state != SessState::Prepared) {
    return false;
  }
  it->second.state = SessState::Publishing;
  /* Response transmission bound: 5s from confirmation. */
  it->second.txDeadlineAt = clockSource().nowMs() + 5000;
  notifyAll(cv_);
  return true;
}

bool SessionTable::finishPublishing(const std::string& id, uint64_t tPrepMs) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.state != SessState::Publishing) {
    return false;
  }
  it->second.state = SessState::Prepared;
  it->second.clientDeadlineAt = clockSource().nowMs() + tPrepMs;
  notifyAll(cv_);
  return true;
}

bool SessionTable::beginTransferring(const std::string& id, uint64_t tExecMs) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.state != SessState::Prepared) {
    return false;
  }
  /* Expiry check shares the lock: a deadline-passed session cannot
   * be revived by a READY (single source of truth). */
  if (clockSource().nowMs() > it->second.clientDeadlineAt) {
    it->second.state = SessState::Reaping;
    notifyAll(cv_);
    return false;
  }
  it->second.state = SessState::Transferring;
  it->second.clientDeadlineAt = clockSource().nowMs() + tExecMs;
  notifyAll(cv_);
  return true;
}

bool SessionTable::beginCompleting(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.state != SessState::Transferring) {
    return false;
  }
  it->second.state = SessState::Completing;
  notifyAll(cv_);
  return true;
}

bool SessionTable::toReaping(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.state == SessState::Reaping) {
    return false;
  }
  it->second.state = SessState::Reaping;
  notifyAll(cv_);
  return true;
}

SessState SessionTable::awaitNotPublishing(const std::string& id,
                                           uint64_t waitDeadlineMs) {
  std::unique_lock<std::mutex> lock(mtx_);
  const auto deadline = std::chrono::steady_clock::time_point(
    std::chrono::milliseconds(waitDeadlineMs));
  cv_.wait_until(lock, deadline, [&] {
    auto it = entries_.find(id);
    return it == entries_.end() || it->second.state != SessState::Publishing;
  });
  auto it = entries_.find(id);
  return it == entries_.end() ? SessState::Reaping : it->second.state;
}

SessState SessionTable::stateOf(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  return it == entries_.end() ? SessState::Reaping : it->second.state;
}

bool SessionTable::claimDestroy(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return false;
  }
  V2Session& s = it->second;
  /* Active handler work is never preempted: the worker's
   * finalizer performs the transition and re-enters the gate. */
  if (s.state != SessState::Reaping || s.ioActive > 0 || s.destroying ||
      (s.destroyClaimed && !s.poisoned)) {
    return false;
  }
  s.destroying = true;
  s.destroyClaimed = true;
  return true;
}

void SessionTable::commitDestroy(const std::string& id, bool qpOk, bool cqOk) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || !it->second.destroying) {
    return;
  }
  V2Session& s = it->second;
  if (qpOk) {
    s.qp = nullptr;
    s.serverQpn = 0;
  }
  if (cqOk) {
    s.cq = nullptr;
  }
  if (s.qp == nullptr && s.cq == nullptr) {
    it->second.ioActive = 0;
    entries_.erase(it);
    notifyAll(cv_);
    return;
  }
  s.poisoned = true;
  s.destroying = false;
}

bool SessionTable::eraseSession(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return false;
  }
  if (it->second.qp != nullptr || it->second.cq != nullptr) {
    return false;
  }
  entries_.erase(it);
  notifyAll(cv_);
  return true;
}

size_t SessionTable::size() const {
  std::lock_guard<std::mutex> guard(mtx_);
  return entries_.size();
}

std::vector<std::string> SessionTable::ids() const {
  std::lock_guard<std::mutex> guard(mtx_);
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto& [id, s] : entries_) {
    out.push_back(id);
  }
  return out;
}

uint64_t SessionTable::ringReserve() {
  std::lock_guard<std::mutex> guard(mtx_);
  return ring_.reserve();
}

void SessionTable::ringUnreserve(uint64_t reservationId) {
  std::lock_guard<std::mutex> guard(mtx_);
  ring_.unreserve(reservationId);
}

void SessionTable::ringRecord(uint64_t reservationId, uint32_t qpn,
                              uint32_t psn) {
  std::lock_guard<std::mutex> guard(mtx_);
  ring_.record(reservationId, qpn, psn);
}

void SessionTable::ringCollectExpired(uint64_t nowMs) {
  std::lock_guard<std::mutex> guard(mtx_);
  ring_.collectExpired(nowMs);
}

bool SessionTable::acquireIo(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return false;
  }
  ++it->second.ioActive;
  return true;
}

int SessionTable::releaseIo(const std::string& id) {
  std::lock_guard<std::mutex> guard(mtx_);
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.ioActive <= 0) {
    return -1;
  }
  return --it->second.ioActive;
}

} // namespace v2
} // namespace hipObj
