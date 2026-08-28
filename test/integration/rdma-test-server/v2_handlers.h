/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Control-protocol handlers for the v2 reference server.
 *
 * PREPARE creates a session (retired-ring slot reserved first),
 * stages GET data, and publishes the server qp/psn. READY runs the
 * data phase and finalizes the session. CANCEL tears a session down
 * idempotently. Every request is SigV4-verified independently and
 * READY/CANCEL additionally compare the credential identity with
 * the session owner. */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "v2_backend.h"
#include "v2_request.h"
#include "v2_session.h"
#include "v2_sigv4.h"

namespace hipObj {
namespace v2 {

struct ServerConfig {
  uint64_t tPrepMs = 10000;
  uint64_t tExecMs = 30000;
};

struct HandlerResult {
  int status = 500;
  std::map<std::string, std::string> headers;
  std::string body;
};

class ControlHandlers {
public:
  ControlHandlers(SigV4Verifier* verifier, MemoryBackend* backend,
                  ServerConfig cfg);
  ~ControlHandlers();

  /* Each handler receives the parsed request plus the raw header
   * block (already part of the request struct through headers). */
  HandlerResult onPrepare(const PrepareRequest& req,
                          const std::string& rawHeaders);
  HandlerResult onReady(const ReadyRequest& req, const std::string& rawHeaders);
  HandlerResult onCancel(const CancelRequest& req,
                         const std::string& rawHeaders);

  SessionTable& table();

  /* Runs one destruction pass over a session: claim under the
   * table lock, destroy owned objects, commit. Sessions whose
   * ioActive > 0 are skipped (the worker finishes them). Exposed
   * for afterSend finalizers and the reaper. */
  void reapSession(const std::string& id);

  /* Response finalizer for a confirmed PREPARE: transitions the
   * session and releases the ioActive reference. */
  void finishPrepareSend(const std::string& id, bool sentOk);

  /* Response finalizer for a confirmed FINAL: transitions to
   * Reaping and releases the ioActive reference. */
  void finishFinalSend(const std::string& id);

private:
  void reaperLoop();

  SigV4Verifier* verifier_;
  MemoryBackend* backend_;
  ServerConfig cfg_;
  SessionTable table_;
  std::thread reaper_;
  std::atomic<bool> reaperStop_{false};
};

} // namespace v2
} // namespace hipObj
