/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_handlers.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "../../../src/common/ibv-wrapper.h"
#include "../../../src/rdma/v2-transport.h"
#include "gid.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "token.h"
#include "v2-clock.h"
#include "v2-random.h"
#include "v2-registry.h"
#include "v2_backend.h"
#include "v2_data_phase.h"
#include "v2_request.h"
#include "v2_sigv4.h"

namespace hipObj {
namespace v2 {

    namespace {

        /* The reference server owns one RDMA device, opened lazily on
         * the first PREPARE. The shared handle carries the context,
         * protection domain, and local GID that per-session connections
         * and memory registrations both use. */
        hipObj::DeviceHandle *serverDevice()
        {
            /* Threaded PREPARE handlers race this lazy init; the once flag
             * serializes the device open and GID query. */
            static hipObj::DeviceHandle *dh = nullptr;
            static std::once_flag        once;
            std::call_once(once, [] {
                int                      n = 0;
                hipObj::IbvDeviceListPtr devs(hipObj::ibv.get_device_list(&n));
                if (devs != nullptr && n > 0) {
                    hipObj::IbvContextPtr ctx(hipObj::ibv.open_device(devs[0]));
                    if (ctx != nullptr) {
                        auto opened = std::make_unique<hipObj::DeviceHandle>();
                        opened->pd.reset(hipObj::ibv.alloc_pd(ctx.get()));
                        opened->portNum  = 1;
                        opened->gidIndex = 0;
                        hipObj::ibv.query_gid(ctx.get(), 1, 0, &opened->localGid);
                        opened->ctx = std::move(ctx);
                        /* Intentionally never freed: the device stays open
                         * for the life of the server. */
                        dh = opened.release();
                    }
                }
            });
            return dh;
        }

        HandlerResult error(int status)
        {
            HandlerResult r;
            r.status = status;
            return r;
        }

        HandlerResult unsupported()
        {
            HandlerResult r;
            r.status                                = 501;
            r.headers["X-Amz-Rdma-Protocol-Status"] = "unsupported";
            return r;
        }

        std::string hex32(uint32_t v)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08" PRIx32, v);
            return buf;
        }

        std::string hex24(uint32_t v)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%06" PRIx32, v);
            return buf;
        }

        /* The success replies are built by helpers that return a single
         * named result. A handler that returns its own named result after
         * returning error() elsewhere can't elide the copy (-Wnrvo). */
        HandlerResult readyReply(uint32_t cookie, uint64_t bytes)
        {
            HandlerResult r;
            r.status                                  = 200;
            r.headers["X-Amz-Rdma-Protocol"]          = "hipobj-rc-v2";
            r.headers["X-Amz-Rdma-Cookie"]            = hex32(cookie);
            r.headers["X-Amz-Rdma-Bytes-Transferred"] = std::to_string(bytes);
            return r;
        }

        HandlerResult cancelReply()
        {
            HandlerResult r;
            r.status = 204;
            return r;
        }

        /* Strips spaces/tabs around a header name (mirrors the parser). */
        std::string trimName(const std::string &s)
        {
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
        bool rdmaHeadersSigned(const std::string &signedList, const std::string &rawHeaders)
        {
            auto isSigned = [&signedList](const std::string &name) {
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
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            size_t pos = 0;
            while (pos < lower.size()) {
                size_t lineEnd = lower.find("\r\n", pos);
                if (lineEnd == std::string::npos) {
                    lineEnd = lower.size();
                }
                std::string line  = lower.substr(pos, lineEnd - pos);
                size_t      colon = line.find(':');
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

    } // namespace

    ControlHandlers::ControlHandlers(SigV4Verifier *verifier, MemoryBackend *backend, ServerConfig cfg)
        : verifier_(verifier), backend_(backend), cfg_(cfg)
    {
        reaper_ = std::thread([this] { reaperLoop(); });
    }

    ControlHandlers::~ControlHandlers()
    {
        reaperStop_.store(true);
        if (reaper_.joinable()) {
            reaper_.join();
        }
        /* Final drain: reap every remaining session so nothing leaks
         * when the server object goes away. */
        for (const auto &id : table_.ids()) {
            table_.toReaping(id);
            reapSession(id);
        }
        /* Sessions still here hold a qp or cq that wouldn't destroy, or
         * are pinned by io. Their members' deleters ignore failure, so
         * don't let the table's destructor free them: work on a live qp
         * may still reference the staging MR and buffer. Relinquish a
         * surviving qp together with its staging; otherwise release the
         * staging normally, which leaks the buffer if the MR won't
         * deregister. */
        for (const auto &id : table_.ids()) {
            table_.withSession(id, [](V2Session &s) {
                if (s.conn.qp != nullptr) {
                    dropOwnership(s.stagingMr);
                    dropOwnership(s.staging);
                }
                else {
                    releaseStaging(s);
                }
                dropOwnership(s.conn.qp);
                dropOwnership(s.conn.cq);
            });
        }
    }

    void ControlHandlers::reapSession(const std::string &id)
    {
        /* Skip sessions with in-flight handler work; their worker's
         * finalizer performs the transition and calls back here. */
        int io = 0;
        table_.withSession(id, [&](V2Session &s) { io = s.ioActive; });
        if (io > 0) {
            return;
        }
        if (!table_.claimDestroy(id)) {
            return;
        }
        /* Destroy owned transport objects. The transport layer owns
         * real qp/cq handles; null pointers mean nothing to destroy. */
        bool     qpOk      = true;
        bool     cqOk      = true;
        uint64_t res       = 0;
        uint32_t qpn       = 0;
        uint32_t psn       = 0;
        bool     published = false;
        /* Destroy the QP/CQ first: a posted work request can still
         * reference the staging MR, so releasing memory before the QP
         * is gone would let the NIC touch freed buffers.
         *
         * The connection is moved out of the session so the destroy
         * verbs run without the table lock. Nothing else touches a
         * claimed session's transport (the claim requires Reaping with
         * no io reference), and the survivors of a failed destroy are
         * moved back below for the retry. */
        hipObj::RcConnV2      conn;
        hipObj::DeviceHandle *s_dev = nullptr;
        table_.withSession(id, [&](V2Session &s) {
            conn  = std::move(s.conn);
            s_dev = s.device;
        });
        bool connRefHeld = false;
        table_.withSession(id, [&](V2Session &s) { connRefHeld = s.connRefHeld; });
        if (conn.qp != nullptr || conn.cq != nullptr) {
            hipObj::v2::destroyRcConnV2(conn, &qpOk, &cqOk);
            if (qpOk && connRefHeld && s_dev != nullptr) {
                /* Consume this session's device reference exactly once:
                 * only a session whose QP was actually created (and now
                 * destroyed) holds one; a CQ-only retry does not. */
                hipObj::v2::releaseDevice(s_dev);
                table_.withSession(id, [&](V2Session &s) { s.connRefHeld = false; });
            }
        }
        table_.withSession(id, [&](V2Session &s) { s.conn = std::move(conn); });
        table_.withSession(id, [&](V2Session &s) {
            res       = s.reservationId;
            qpn       = s.serverQpn;
            psn       = s.serverPsn;
            published = s.published;
            /* Safe only when the QP is gone: a live QP can still hold
             * work requests referencing the staging MR. A failed destroy
             * leaves the MR and buffer on the session for the retry. */
            if (qpOk) {
                releaseStaging(s);
            }
        });
        /* Retire the slot exactly once: a QP that was destroyed (or
         * never had one wired) settles the reservation now; a failed
         * destroy keeps the reservation for the retry. */
        if (qpOk && res != 0) {
            if (published) {
                table_.ringRecord(res, qpn, psn);
            }
            else {
                table_.ringUnreserve(res);
            }
            table_.withSession(id, [&](V2Session &s) {
                s.reservationId = 0; /* settled; retries see no slot */
            });
        }
        table_.commitDestroy(id, qpOk, cqOk);
    }

    void ControlHandlers::finishPrepareSend(const std::string &id, bool sentOk)
    {
        if (sentOk) {
            table_.finishPublishing(id, cfg_.tPrepMs);
        }
        else {
            table_.toReaping(id);
        }
        table_.releaseIo(id);
        reapSession(id);
    }

    void ControlHandlers::finishFinalSend(const std::string &id)
    {
        table_.toReaping(id);
        table_.releaseIo(id);
        reapSession(id);
    }

    void ControlHandlers::reaperLoop()
    {
        while (!reaperStop_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            table_.ringCollectExpired(clockSource().nowMs());
            uint64_t now = clockSource().nowMs();
            for (const auto &id : table_.ids()) {
                table_.withSession(id, [&](V2Session &s) {
                    /* Client-lifetime expiry: no READY arrived in time, or the
                     * transfer outlived T_exec. */
                    if ((s.state == SessState::Prepared || s.state == SessState::Transferring) &&
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
                    /* Force-release only references still waiting for a
                     * Publishing response (worker died or the response never
                     * left). A reference taken past Completing guards live
                     * staging data and is released by the cooperative
                     * finalizer; the origin flag tells the two apart even
                     * after a concurrent CANCEL moved the state to
                     * Reaping. */
                    if (!s.ioFromCompleting && s.txDeadlineAt != 0 && now > s.txDeadlineAt &&
                        s.ioActive > 0) {
                        --s.ioActive;
                    }
                    if (s.txDeadlineAt != 0 && now > s.txDeadlineAt && s.ioActive == 0) {
                        s.state = SessState::Reaping;
                    }
                });
                reapSession(id);
            }
        }
    }
    SessionTable &ControlHandlers::table()
    {
        return table_;
    }

    HandlerResult ControlHandlers::onPrepare(const PrepareRequest &req, const std::string &rawHeaders)
    {
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
        bool        inserted = false;
        for (int attempt = 0; attempt < 3 && !inserted; ++attempt) {
            uint32_t w[4] = {0, 0, 0, 0};
            if (!randomSource().next32(w[0]) || !randomSource().next32(w[1]) ||
                !randomSource().next32(w[2]) || !randomSource().next32(w[3])) {
                table_.ringUnreserve(slot);
                return error(500);
            }
            char buf[33];
            std::snprintf(buf, sizeof(buf), "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32, w[0], w[1],
                          w[2], w[3]);
            id = buf;
            V2Session s;
            s.id        = id;
            s.op        = req.op;
            s.target    = req.target;
            s.size      = req.size;
            s.offset    = req.offset;
            s.cookie    = req.cookie;
            s.clientPsn = req.clientPsn;
            /* Decode the client's 88-hex token. The all-zero token is the
             * explicit same-HCA loopback marker (keep the local GID
             * fallback); any other token must decode, and its GID becomes
             * the RTR routing target. */
            const std::string tokBase = req.token.substr(0, 88);
            if (tokBase == std::string(88, '0')) {
                s.hasPeerGid = false;
            }
            else {
                hipObj::RdmaToken peerTok;
                if (!hipObj::decodeRdmaTokenHex(tokBase.c_str(), peerTok) ||
                    /* Semantic checks: this server pairs RC transports only,
                     * and a nonzero token must carry a real peer GID. */
                    peerTok.transport != hipObj::TRANSPORT_RC || peerTok.gid == hipObj::Gid{}) {
                    table_.ringUnreserve(slot);
                    return error(400);
                }
                s.peerGid    = hipObj::toIbvGid(peerTok.gid);
                s.hasPeerGid = true;
            }
            s.accessKey        = cred->accessKey;
            s.reservationId    = slot;
            s.clientDeadlineAt = clockSource().nowMs() + cfg_.tPrepMs;
            inserted           = table_.insert(std::move(s));
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
        table_.withSession(id, [&](V2Session &s) { s.serverPsn = serverPsn; });

        /* Per-session connection on the shared device: one qp/cq pair
         * the client pairs against using the qpn/psn in the reply. A
         * device-less host keeps the session transport-free and the
         * data phase degrades to the verified no-op. */
        hipObj::DeviceHandle *dh = serverDevice();
        if (dh != nullptr && dh->pd != nullptr) {
            hipObj::RcConnV2 conn;
            bool             rollbackFailed = false;
            bool             created        = hipObj::v2::createRcConnV2(dh, conn, &rollbackFailed) == 0;
            if (created &&
                /* The pair starts in RESET; move it to INIT so the READY
                 * RTR/RTS transitions are valid. */
                hipObj::v2::transitionQpToInitV2(dh, conn) != 0) {
                /* INIT failed: destroy the pair here or its handles leak
                 * on every PREPARE. A handle that fails to destroy stays
                 * on the session so the reaper keeps retrying it. */
                bool qOk = true, cOk = true;
                hipObj::v2::destroyRcConnV2(conn, &qOk, &cOk);
                if (qOk) {
                    /* The QP existed and destroyed cleanly: consume this
                     * session's device reference right here - the reaper
                     * will find no handle and must not release again. */
                    hipObj::v2::releaseDevice(dh);
                }
                if (!qOk || !cOk) {
                    /* The surviving handles move to the session in the
                     * failure branch below. */
                    table_.withSession(id, [&](V2Session &s) {
                        /* A surviving QP keeps the reference alive for the
                         * reaper retry - so the session must still claim
                         * ownership of it. */
                        s.connRefHeld = !qOk;
                        s.device      = dh;
                    });
                }
                else {
                    table_.withSession(id, [&](V2Session &s) { s.connRefHeld = false; });
                }
                created = false;
            }
            if (created) {
                table_.withSession(id, [&](V2Session &s) {
                    s.serverQpn   = conn.qpNum;
                    s.conn        = std::move(conn);
                    s.device      = dh;
                    s.connRefHeld = true;
                });
            }
            else {
                /* Any failure - clean rollback, partial rollback, or a
                 * failed INIT - ends the PREPARE here. Surviving handles
                 * move to the session for the reaper; destroyed ones are
                 * already null. */
                table_.withSession(id, [&](V2Session &s) {
                    if (conn.qp != nullptr) {
                        s.serverQpn = conn.qpNum;
                    }
                    s.conn   = std::move(conn);
                    s.device = dh;
                });
                table_.toReaping(id);
                table_.releaseIo(id);
                table_.ringUnreserve(slot);
                return error(500);
            }
        }

        /* PUT: stage an empty buffer the client writes into; the
         * endpoint is exposed in the PREPARE reply. */
        if (req.op == "PUT" && dh != nullptr && dh->pd != nullptr) {
            /* Allocate + register outside the table lock; only the
             * session-field update takes it. */
            V2Session stageScratch;
            stageScratch.op   = req.op;
            stageScratch.size = req.size;
            bool staged       = stagePutBuffer(stageScratch, static_cast<size_t>(req.size), dh->pd.get());
            if (staged) {
                table_.withSession(id, [&](V2Session &s) {
                    s.staging   = std::move(stageScratch.staging);
                    s.stagingMr = std::move(stageScratch.stagingMr);
                });
            }
            if (!staged) {
                table_.toReaping(id);
                table_.releaseIo(id);
                table_.ringUnreserve(slot);
                return error(500);
            }
        }

        /* GET: the object must exist and cover the requested range.
         * The object data is staged into a registered MR here so the
         * READY data phase can READ straight out of it. */
        if (req.op == "GET") {
            if (!backend_->has(req.target)) {
                table_.toReaping(id);
                table_.releaseIo(id);
                table_.ringUnreserve(slot);
                return error(500);
            }
            /* Stage the object into the session buffer. The backend copy
             * runs on local state - holding the table lock across it
             * would serialize every other request behind this one. */
            struct ibv_pd *pd = (dh != nullptr && dh->pd != nullptr) ? dh->pd.get() : nullptr;
            V2Session      stageScratch;
            stageScratch.op   = req.op;
            stageScratch.size = req.size;
            bool  staged      = stagePutBuffer(stageScratch, static_cast<size_t>(req.size), pd);
            void *staging     = stageScratch.staging.get();
            bool  copied      = staged && staging != nullptr &&
                          backend_->read(req.target, req.offset, static_cast<size_t>(req.size), staging);
            if (staged) {
                table_.withSession(id, [&](V2Session &s) {
                    s.staging   = std::move(stageScratch.staging);
                    s.stagingMr = std::move(stageScratch.stagingMr);
                });
            }
            /* A missing range on an existing object is a client error;
             * staging without a PD is fine on transport-free hosts where
             * the data phase is a no-op. */
            if (!staged || (!copied && req.size > 0)) {
                table_.toReaping(id);
                table_.releaseIo(id);
                table_.ringUnreserve(slot);
                return error(staged ? 416 : 500);
            }
        }

        /* Publish: response confirmed under the table lock below via
         * beginPublishing; published=true marks the tuple as exposed. */
        if (!table_.beginPublishing(id)) {
            table_.toReaping(id);
            table_.releaseIo(id);
            table_.ringUnreserve(slot);
            return error(500);
        }
        table_.withSession(id, [&](V2Session &s) { s.published = true; });

        return prepareReply(id, serverPsn);
    }

    HandlerResult ControlHandlers::prepareReply(const std::string &id, uint32_t serverPsn)
    {
        HandlerResult r;
        r.status                         = 200;
        r.headers["X-Amz-Rdma-Protocol"] = "hipobj-rc-v2";
        /* Reply token carrying this server's endpoint so a remote
         * peer can route back: QPN/GID from the paired session. */
        {
            hipObj::RdmaToken     replyTok{};
            uint32_t              replyQpn = 0;
            hipObj::DeviceHandle *replyDev = serverDevice();
            table_.withSession(id, [&](V2Session &s) {
                replyQpn = s.serverQpn;
                replyDev = s.device != nullptr ? s.device : replyDev;
            });
            if (replyQpn != 0 && replyDev != nullptr) {
                replyTok.qpNum                = replyQpn;
                replyTok.gid                  = hipObj::toGid(replyDev->localGid);
                replyTok.transport            = hipObj::TRANSPORT_RC;
                replyTok.portNum              = replyDev->portNum;
                r.headers["X-Amz-Rdma-Reply"] = "200:" + hipObj::encodeRdmaToken(replyTok);
            }
            else {
                /* Control-plane-only exchange: no transport to describe. */
                r.headers["X-Amz-Rdma-Reply"] = "200:" + std::string(88, '0');
            }
        }
        r.headers["X-Amz-Rdma-Session"] = id;
        r.headers["X-Amz-Rdma-Psn"]     = hex24(serverPsn);
        uint32_t exposedQpn             = 0;
        table_.withSession(id, [&](V2Session &s) { exposedQpn = s.serverQpn; });
        if (exposedQpn != 0) {
            char qpnHex[12];
            std::snprintf(qpnHex, sizeof(qpnHex), "%" PRIx32, exposedQpn);
            r.headers["X-Amz-Rdma-Qpn"] = qpnHex;
            /* Staging endpoint for the client's WRITE (PUT) or the READ
             * pull confirmation (GET). */
            uint64_t saddr = 0;
            uint32_t srkey = 0;
            table_.withSession(id, [&](V2Session &s) {
                if (s.stagingMr != nullptr) {
                    saddr = reinterpret_cast<uintptr_t>(s.stagingMr->addr);
                    srkey = s.stagingMr->rkey;
                }
            });
            if (saddr != 0) {
                char addrHex[32];
                std::snprintf(addrHex, sizeof(addrHex), "%" PRIx64, saddr);
                char rkeyHex[12];
                std::snprintf(rkeyHex, sizeof(rkeyHex), "%" PRIx32, srkey);
                r.headers["X-Amz-Rdma-Mr-Addr"] = addrHex;
                r.headers["X-Amz-Rdma-Mr-Rkey"] = rkeyHex;
            }
        }
        return r;
    }

    HandlerResult ControlHandlers::onReady(const ReadyRequest &req, const std::string &rawHeaders)
    {
        if (req.protocol != "hipobj-rc-v2") {
            return unsupported();
        }
        auto cred = verifier_->verify("POST", "/.hipobj-rc/ready", rawHeaders, "");
        if (!cred.has_value() || !rdmaHeadersSigned(cred->signedHeaders, rawHeaders)) {
            return error(403);
        }

        SessState st = table_.stateOf(req.session);
        if (st == SessState::Publishing) {
            uint64_t deadline = 0;
            table_.withSession(req.session, [&](V2Session &s) { deadline = s.txDeadlineAt; });
            st = table_.awaitNotPublishing(req.session, deadline);
        }

        switch (st) {
            case SessState::Prepared:
                break; /* proceed */
            case SessState::Transferring:
            case SessState::Completing:
                return error(409); /* duplicate or in-flight FINAL */
            /* Still Publishing means the wait above hit the deadline */
            case SessState::Publishing:
            case SessState::Reaping:
            default:
                /* Stale/expired: terminal 409 (session is already doomed). */
                return error(409);
        }

        /* Credential identity + cookie must match the session. */
        bool authOk   = false;
        bool cookieOk = false;
        table_.withSession(req.session, [&](V2Session &s) {
            authOk   = s.accessKey == cred->accessKey;
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

        /* Record the client wire endpoint from the READY headers and
         * pair the session QP to the client QP (INIT was done at
         * creation; RTR/RTS complete the handshake using the client
         * PSN recorded at PREPARE). */
        bool paired = true;
        table_.withSession(req.session, [&](V2Session &s) {
            s.clientMrAddr = req.mrAddr;
            s.clientMrRkey = req.mrRkey;
            s.clientQpn    = req.qpn;
            if (s.conn.qp != nullptr && req.qpn != 0) {
                /* A device-less session pairs with an empty handle */
                hipObj::DeviceHandle noDevice;
                noDevice.gidIndex        = 0;
                hipObj::DeviceHandle *dh = s.device != nullptr ? s.device : &noDevice;
                /* Same-HCA pairing: the server's own GID routes the
                 * loopback path on RoCE devices. */
                /* Route to the real peer when the token carried its GID;
                 * same-HCA loopback (zero token) keeps the local GID. */
                union ibv_gid peer = s.hasPeerGid ? s.peerGid : dh->localGid;
                paired = hipObj::v2::transitionQpToRtrV2(dh, s.conn, req.qpn, peer, s.clientPsn) == 0 &&
                         hipObj::v2::transitionQpToRtsV2(s.conn, dh, s.serverPsn) == 0;
            }
        });
        if (!paired) {
            table_.toReaping(req.session);
            table_.releaseIo(req.session);
            return error(500);
        }

        /* Data phase bounded by the transition deadline. The result
         * feeds FINAL directly; failures release the io reference here
         * because no response finalizer will run for them. */
        DataPhaseStats stats;
        uint64_t       deadline = 0;
        DataPhaseView  view;
        table_.withSession(req.session, [&](V2Session &s) {
            deadline          = s.clientDeadlineAt;
            view.op           = s.op;
            view.size         = s.size;
            view.cookie       = s.cookie;
            view.qp           = s.conn.qp.get();
            view.cq           = s.conn.cq.get();
            view.stagingMr    = s.stagingMr.get();
            view.clientMrAddr = s.clientMrAddr;
            view.clientMrRkey = s.clientMrRkey;
            view.clientQpn    = s.clientQpn;
        });
        /* The transfer runs on a local snapshot: polling the CQ can
         * block for the whole T_exec window, and holding the session
         * table lock for that would stall every other request and the
         * reaper. The io reference taken above keeps the session (and
         * these objects) alive for the duration. */
        DataPhaseResult dpr = runDataPhase(view, deadline, stats);
        if (dpr != DataPhaseResult::Ok) {
            table_.toReaping(req.session);
            table_.releaseIo(req.session);
            return dpr == DataPhaseResult::Timeout ? error(408) : error(500);
        }
        /* Claim the Completing transition BEFORE persisting: once
         * the state check passes, a concurrent CANCEL or the reaper
         * can no longer flip the session behind our back, and a 500
         * path never leaves a stored object behind. The io reference
         * is still held: it pins the session (and the staging buffer)
         * against the reaper until the write below finishes. */
        if (!table_.beginCompleting(req.session)) {
            table_.releaseIo(req.session);
            return error(500);
        }

        if (view.op == "PUT") {
            /* Persist the uploaded bytes. A control-plane-only exchange
             * (no client QP advertised) never transferred anything, so
             * there is nothing valid to store - the staged buffer is
             * either null or uninitialized. The write runs on local
             * state; only the session fields are lock-protected. */
            std::string target;
            void       *staging          = nullptr;
            uint32_t    sessionClientQpn = 0;
            table_.withSession(req.session, [&](V2Session &s) {
                target           = s.target;
                staging          = s.staging.get();
                sessionClientQpn = s.clientQpn;
            });
            if (sessionClientQpn != 0 && staging != nullptr) {
                backend_->write(target, staging, stats.bytes);
            }
        }

        return readyReply(req.cookie, stats.bytes);
    }

    HandlerResult ControlHandlers::onCancel(const CancelRequest &req, const std::string &rawHeaders)
    {
        if (req.protocol != "hipobj-rc-v2") {
            return unsupported();
        }
        auto cred = verifier_->verify("POST", "/.hipobj-rc/cancel", rawHeaders, "");
        if (!cred.has_value() || !rdmaHeadersSigned(cred->signedHeaders, rawHeaders)) {
            return error(403);
        }
        /* Existence and credential match in one look-up: a session
         * erased between the two reads must still answer 204 (the work
         * is done), never 403. */
        bool owned = false;
        table_.withSession(req.session, [&](V2Session &s) { owned = s.accessKey == cred->accessKey; });
        if (owned) {
            table_.toReaping(req.session);
        }
        /* Idempotent either way: absent, foreign (still 204 to avoid
         * probing), or reaped. */
        return cancelReply();
    }

} // namespace v2
} // namespace hipObj
