/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_handlers.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "v2-clock.h"
#include "v2-random.h"
#include "v2-registry.h"

namespace hipObj {
namespace v2 {

namespace {} // namespace

HandlerResult error(int status) {
  HandlerResult r;
  r.status = status;
  return r;
}

HandlerResult unsupported() {
  HandlerResult r;
  r.status = 501;
  r.headers["X-Amz-Rdma-Protocol-Status"] = "unsupported";
  return r;
}

std::string hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return buf;
}

std::string hex24(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%06x", v);
  return buf;
}

/* Strips spaces/tabs around a header name (mirrors the parser). */
std::string trimName(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
    --e;
  }
  return s.substr(b, e - b);
}

/* All rdma headers present in the raw request must appear in the
 * signed-headers list (unsigned protocol fields would let a proxy
 * strip them invisibly). Membership is an exact token match on
 * the semicolon-separated list, not a substring test. */
bool rdmaHeadersSigned(const std::string& signedList,
                       const std::string& rawHeaders) {
  auto isSigned = [&signedList](const std::string& name) {
    size_t pos = 0;
    while (pos < signedList.size()) {
      size_t end = signedList.find(';', pos);
      if (end == std::string::npos) {
        end = signedList.size();
      }
      if (signedList.compare(pos, end - pos, name) == 0) {
        return true;
      }
      pos = end + 1;
    }
    return false;
  };
  std::string lower;
  lower.reserve(rawHeaders.size());
  for (char c : rawHeaders) {
    lower.push_back(
      static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  size_t pos = 0;
  while (pos < lower.size()) {
    size_t lineEnd = lower.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      lineEnd = lower.size();
    }
    std::string line = lower.substr(pos, lineEnd - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      /* Trim like the HTTP parser does: a leading-space name must
       * not slip past the signed-header check. */
      std::string name = trimName(line.substr(0, colon));
      if (name.rfind("x-amz-rdma-", 0) == 0 && !isSigned(name)) {
        return false;
      }
    }
    pos = lineEnd + 2;
  }
  return true;
}

ControlHandlers::ControlHandlers(SigV4Verifier* verifier,
                                 MemoryBackend* backend, ServerConfig cfg)
  : verifier_(verifier), backend_(backend), cfg_(cfg) {
  reaper_ = std::thread([this] {
    reaperLoop();
  });
}

ControlHandlers::~ControlHandlers() {
  reaperStop_.store(true);
  if (reaper_.joinable()) {
    reaper_.join();
  }
  /* Final drain: reap every remaining session so nothing leaks
   * when the server object goes away. */
  for (const auto& id : table_.ids()) {
    table_.toReaping(id);
    reapSession(id);
  }
}

void ControlHandlers::reapSession(const std::string& id) {
  /* Skip sessions with in-flight handler work; their worker's
   * finalizer performs the transition and calls back here. */
  int io = 0;
  table_.withSession(id, [&](V2Session& s) {
    io = s.ioActive;
  });
  if (io > 0) {
    return;
  }
  if (!table_.claimDestroy(id)) {
    return;
  }
  /* Destroy owned transport objects. The transport layer owns
   * real qp/cq handles; null pointers mean nothing to destroy. */
  bool qpOk = true;
  bool cqOk = true;
  uint64_t res = 0;
  uint32_t qpn = 0;
  uint32_t psn = 0;
  bool published = false;
  table_.withSession(id, [&](V2Session& s) {
    res = s.reservationId;
    qpn = s.serverQpn;
    psn = s.serverPsn;
    published = s.published;
    /* The transport layer owns real qp/cq handles; null pointers
     * mean nothing to destroy yet and the commit erases the
     * entry. A non-null handle without a wired destroy reports
     * failure so the entry stays poisoned for the transport
     * follow-up. */
    if (s.qp != nullptr) {
      qpOk = false;
    }
    if (s.cq != nullptr) {
      cqOk = false;
    }
  });
  /* Retire the slot exactly once: a QP that was destroyed (or
   * never had one wired) settles the reservation now; a failed
   * destroy keeps the reservation for the retry. */
  if (qpOk && res != 0) {
    if (published) {
      table_.ringRecord(res, qpn, psn);
    } else {
      table_.ringUnreserve(res);
    }
    table_.withSession(id, [&](V2Session& s) {
      s.reservationId = 0; /* settled; retries see no slot */
    });
  }
  table_.commitDestroy(id, qpOk, cqOk);
}

void ControlHandlers::finishPrepareSend(const std::string& id, bool sentOk) {
  if (sentOk) {
    table_.finishPublishing(id, cfg_.tPrepMs);
  } else {
    table_.toReaping(id);
  }
  table_.releaseIo(id);
  reapSession(id);
}

void ControlHandlers::finishFinalSend(const std::string& id) {
  table_.toReaping(id);
  table_.releaseIo(id);
  reapSession(id);
}

void ControlHandlers::reaperLoop() {
  while (!reaperStop_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    table_.ringCollectExpired(clockSource().nowMs());
    uint64_t now = clockSource().nowMs();
    for (const auto& id : table_.ids()) {
      table_.withSession(id, [&](V2Session& s) {
        /* Client-lifetime expiry: no READY arrived in time, or the
         * transfer outlived T_exec. */
        if ((s.state == SessState::Prepared ||
             s.state == SessState::Transferring) &&
            now > s.clientDeadlineAt) {
          s.state = SessState::Reaping;
          return;
        }
        /* Response-bound expiry: the worker never finished sending
         * the confirmed response - including sessions moved to
         * Reaping by a concurrent CANCEL before this sweep. Force
         * the reference release so the claim gate below can
         * reclaim the session and its ring slot; a later
         * cooperative finalizer releasing an erased id is a
         * no-op. */
        if (s.txDeadlineAt != 0 && now > s.txDeadlineAt && s.ioActive > 0) {
          --s.ioActive;
        }
        if ((s.state == SessState::Publishing ||
             s.state == SessState::Completing) &&
            now > s.txDeadlineAt) {
          s.state = SessState::Reaping;
        }
      });
      reapSession(id);
    }
  }
}
SessionTable& ControlHandlers::table() {
  return table_;
}

HandlerResult ControlHandlers::onPrepare(const PrepareRequest& req,
                                         const std::string& rawHeaders) {
  /* Protocol echo is mandatory: without it the client is speaking
   * something else, answer with the explicit unsupported marker. */
  if (req.protocol != "hipobj-rc-v2") {
    return unsupported();
  }

  auto cred = verifier_->verify("POST", "/.hipobj-rc/prepare", rawHeaders, "");
  if (!cred.has_value()) {
    return error(403);
  }
  /* Size cap: syntactically valid but over the transfer limit. */
  if (req.size > 0x7fffffff) {
    return error(413);
  }
  /* Every rdma header carried by the request must be signed. */
  if (!rdmaHeadersSigned(cred->signedHeaders, rawHeaders)) {
    return error(403);
  }

  /* Slot-first: the retired ring must have room before the session
   * or any object exists. */
  uint64_t slot = table_.ringReserve();
  if (slot == 0) {
    return error(503);
  }

  /* Session id: 4 draws, insert-if-absent, up to 3 redraws. */
  std::string id;
  bool inserted = false;
  for (int attempt = 0; attempt < 3 && !inserted; ++attempt) {
    uint32_t w[4] = {0, 0, 0, 0};
    if (!randomSource().next32(w[0]) || !randomSource().next32(w[1]) ||
        !randomSource().next32(w[2]) || !randomSource().next32(w[3])) {
      table_.ringUnreserve(slot);
      return error(500);
    }
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", w[0], w[1], w[2], w[3]);
    id = buf;
    V2Session s;
    s.id = id;
    s.op = req.op;
    s.target = req.target;
    s.size = req.size;
    s.offset = req.offset;
    s.cookie = req.cookie;
    s.clientPsn = req.clientPsn;
    s.accessKey = cred->accessKey;
    s.reservationId = slot;
    s.clientDeadlineAt = clockSource().nowMs() + cfg_.tPrepMs;
    inserted = table_.insert(std::move(s));
  }
  if (!inserted) {
    table_.ringUnreserve(slot);
    return error(503);
  }

  /* Server PSN draw. */
  uint32_t serverPsn = 0;
  if (!nextClientPsn(serverPsn)) {
    table_.toReaping(id);
    table_.releaseIo(id);
    table_.ringUnreserve(slot);
    return error(500);
  }
  table_.withSession(id, [&](V2Session& s) {
    s.serverPsn = serverPsn;
  });

  /* GET: the object must exist and cover the requested range.
   * Staging into an MR happens in the transport layer; here the
   * presence check gates session creation. */
  if (req.op == "GET" && !backend_->has(req.target)) {
    table_.toReaping(id);
    table_.releaseIo(id);
    table_.ringUnreserve(slot);
    return error(500);
  }

  /* Publish: response confirmed under the table lock below via
   * beginPublishing; published=true marks the tuple as exposed. */
  if (!table_.beginPublishing(id)) {
    table_.toReaping(id);
    table_.releaseIo(id);
    table_.ringUnreserve(slot);
    return error(500);
  }
  table_.withSession(id, [&](V2Session& s) {
    s.published = true;
  });

  HandlerResult r;
  r.status = 200;
  r.headers["X-Amz-Rdma-Protocol"] = "hipobj-rc-v2";
  /* Reply token: server side placeholder until the transport
   * commit fills the real 88-hex value. */
  r.headers["X-Amz-Rdma-Reply"] = "200:" + std::string(88, '0');
  r.headers["X-Amz-Rdma-Session"] = id;
  r.headers["X-Amz-Rdma-Psn"] = hex24(serverPsn);
  return r;
}

HandlerResult ControlHandlers::onReady(const ReadyRequest& req,
                                       const std::string& rawHeaders) {
  if (req.protocol != "hipobj-rc-v2") {
    return unsupported();
  }
  auto cred = verifier_->verify("POST", "/.hipobj-rc/ready", rawHeaders, "");
  if (!cred.has_value() ||
      !rdmaHeadersSigned(cred->signedHeaders, rawHeaders)) {
    return error(403);
  }

  SessState st = table_.stateOf(req.session);
  if (st == SessState::Publishing) {
    uint64_t deadline = 0;
    table_.withSession(req.session, [&](V2Session& s) {
      deadline = s.txDeadlineAt;
    });
    st = table_.awaitNotPublishing(req.session, deadline);
  }

  switch (st) {
    case SessState::Prepared:
      break; /* proceed */
    case SessState::Transferring:
    case SessState::Completing:
      return error(409); /* duplicate or in-flight FINAL */
    case SessState::Reaping:
    default:
      /* Stale/expired: terminal 409 (session is already doomed). */
      return error(409);
  }

  /* Credential identity + cookie must match the session. */
  bool authOk = false;
  bool cookieOk = false;
  table_.withSession(req.session, [&](V2Session& s) {
    authOk = s.accessKey == cred->accessKey;
    cookieOk = s.cookie == req.cookie;
  });
  if (!authOk || !cookieOk) {
    /* Preserve-errors: the session stays untouched. */
    return error(403);
  }

  /* Hold the handler reference for the whole READY: the reaper
   * cannot claim the session while the data phase runs. The
   * response finalizer (finishFinalSend) releases it. */
  if (!table_.acquireIo(req.session)) {
    return error(409);
  }
  if (!table_.beginTransferring(req.session, cfg_.tExecMs)) {
    table_.releaseIo(req.session);
    return error(409); /* expired under the lock */
  }

  /* Data phase: the transport commit wires WRITE/READ posts here.
   * For the session/handler layer, finalize the flow now - the
   * result reflects a completed transfer. A failed transition
   * means the session was reaped mid-flight: report failure and
   * release the reference here (no finalizer runs for errors). */
  if (!table_.beginCompleting(req.session)) {
    table_.releaseIo(req.session);
    return error(500);
  }

  uint64_t bytes = 0;
  table_.withSession(req.session, [&](V2Session& s) {
    bytes = s.size;
  });

  HandlerResult r;
  r.status = 200;
  r.headers["X-Amz-Rdma-Protocol"] = "hipobj-rc-v2";
  r.headers["X-Amz-Rdma-Cookie"] = hex32(req.cookie);
  r.headers["X-Amz-Rdma-Bytes-Transferred"] = std::to_string(bytes);
  return r;
}

HandlerResult ControlHandlers::onCancel(const CancelRequest& req,
                                        const std::string& rawHeaders) {
  if (req.protocol != "hipobj-rc-v2") {
    return unsupported();
  }
  auto cred = verifier_->verify("POST", "/.hipobj-rc/cancel", rawHeaders, "");
  if (!cred.has_value() ||
      !rdmaHeadersSigned(cred->signedHeaders, rawHeaders)) {
    return error(403);
  }
  /* Existence and credential match in one look-up: a session
   * erased between the two reads must still answer 204 (the work
   * is done), never 403. */
  bool owned = false;
  table_.withSession(req.session, [&](V2Session& s) {
    owned = s.accessKey == cred->accessKey;
  });
  if (owned) {
    table_.toReaping(req.session);
  }
  /* Idempotent either way: absent, foreign (still 204 to avoid
   * probing), or reaped. */
  HandlerResult r;
  r.status = 204;
  return r;
}

} // namespace v2
} // namespace hipObj
