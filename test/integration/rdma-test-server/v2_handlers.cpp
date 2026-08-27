/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_handlers.h"

#include <cstdio>

#include "v2-clock.h"
#include "v2-random.h"
#include "v2-registry.h"

namespace hipObj {
namespace v2 {

namespace {

/* Server-local retired ring guarding (qpn, psn) reuse on the
 * server side (separate instance from the client's). */
RetiredRing& serverRing() {
  static RetiredRing ring;
  return ring;
}

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

/* All rdma headers present in the request must be signed. */
bool rdmaHeadersSigned(const std::string& signedList,
                       const std::map<std::string, std::string>& hdrs,
                       const std::string& except) {
  for (const auto& [k, v] : hdrs) {
    if (k.rfind("x-amz-rdma-", 0) == 0 && k != except &&
        signedList.find(k) == std::string::npos) {
      return false;
    }
  }
  return true;
}

} // namespace

ControlHandlers::ControlHandlers(SigV4Verifier* verifier,
                                 MemoryBackend* backend, ServerConfig cfg)
  : verifier_(verifier), backend_(backend), cfg_(cfg) {
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
  if (!rdmaHeadersSigned(cred->signedHeaders, {}, "")) {
    return error(403);
  }

  /* Slot-first: the retired ring must have room before the session
   * or any object exists. */
  uint64_t slot = serverRing().reserve();
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
      serverRing().unreserve(slot);
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
    serverRing().unreserve(slot);
    return error(503);
  }

  /* Server PSN draw. */
  uint32_t serverPsn = 0;
  if (!nextClientPsn(serverPsn)) {
    table_.toReaping(id);
    serverRing().unreserve(slot);
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
    serverRing().unreserve(slot);
    return error(500);
  }

  /* Publish: response confirmed under the table lock below via
   * beginPublishing; published=true marks the tuple as exposed. */
  if (!table_.beginPublishing(id)) {
    table_.toReaping(id);
    serverRing().unreserve(slot);
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
  auto cred = verifier_->verify("POST", "/.hipobj-rc/ready", rawHeaders, "");
  if (!cred.has_value()) {
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

  if (!table_.beginTransferring(req.session, cfg_.tExecMs)) {
    return error(409); /* expired under the lock */
  }

  /* Data phase: the transport commit wires WRITE/READ posts here.
   * For the session/handler layer, finalize the flow now - the
   * result reflects a completed transfer. */
  table_.beginCompleting(req.session);

  uint64_t bytes = 0;
  std::string etag;
  table_.withSession(req.session, [&](V2Session& s) {
    bytes = s.size;
  });
  table_.withSession(req.session, [&](V2Session& s) {
    s.txDeadlineAt = clockSource().nowMs() + 5000;
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
  auto cred = verifier_->verify("POST", "/.hipobj-rc/cancel", rawHeaders, "");
  if (!cred.has_value()) {
    return error(403);
  }
  bool exists = table_.withSession(req.session, [](V2Session&) {
  });
  if (exists) {
    bool authOk = false;
    table_.withSession(req.session, [&](V2Session& s) {
      authOk = s.accessKey == cred->accessKey;
    });
    if (!authOk) {
      return error(403);
    }
    table_.toReaping(req.session);
  }
  /* Idempotent either way. */
  HandlerResult r;
  r.status = 204;
  return r;
}

} // namespace v2
} // namespace hipObj
