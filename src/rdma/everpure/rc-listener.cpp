/* SPDX-License-Identifier: MIT
 */

#include "rc-listener.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rdma/rdma_cma.h>

#include "log.h"
#include "pd-lock.h"

namespace hipObj {

    std::mutex RdmaCmRcListener::s_registryMutex;
    std::vector<RdmaCmRcListener*> RdmaCmRcListener::s_listeners;

    RdmaCmRcListener::~RdmaCmRcListener() = default;

    void RdmaCmRcListener::DestroyConnection(struct rdma_cm_id* id) {
        if (id->qp) {
            struct ibv_qp* qp = id->qp;
            struct ibv_cq* sendCq = qp->send_cq;
            {
                PdLockGuard pdLock;
                rdma_destroy_qp(id);
                if (sendCq) {
                    ibv_destroy_cq(sendCq);
                }
            }
        }
        rdma_destroy_id(id);
    }

    void RdmaCmRcListener::RemoveActiveConn(struct rdma_cm_id* id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = activeConns_.begin(); it != activeConns_.end(); ++it) {
            if (*it == id) {
                activeConns_.erase(it);
                return;
            }
        }
    }

    /* CONNECT_REQUEST: build a QP on our existing PD (so it can service
     * RDMA READ/WRITE against MRs registered there) and accept. RDMA CM
     * handles the INIT/RTR/RTS transitions itself as part of accept. */
    void RdmaCmRcListener::HandleConnectRequest(struct rdma_cm_id* id) {
        /* Start() took deviceCtx_ from the listen id, so every connection
         * RDMA CM accepts on it should arrive on the same context. Kept as a
         * guard: a QP, CQ or MR from one context can't be used through
         * another, even on the same device. */
        if (id->verbs != deviceCtx_) {
            LOG_ERROR(
                "rc-listener: rejecting CONNECT_REQUEST: it arrived on %s:%p, "
                "but the listener's PD and CQs are on %s:%p",
                id->verbs ? ibv_get_device_name(id->verbs->device) : "(null)",
                static_cast<void*>(id->verbs),
                deviceCtx_ ? ibv_get_device_name(deviceCtx_->device) : "(null)",
                static_cast<void*>(deviceCtx_));
            rdma_reject(id, nullptr, 0);
            rdma_destroy_id(id);
            return;
        }

        /* TODO: limit the connections. Every connect request gets a CQ and
         * a QP here, with no cap and no timeout, so any host that can reach
         * the port can use up the verbs resources. Cap how many can be
         * active at once, rejecting a request here before its CQ is
         * created, and tear down connections older than a time limit. */

        /* hipObject never posts its own send/recv on this QP -- the peer
         * performs one-sided RDMA READ/WRITE against our MRs, which
         * generates no local completion -- so this CQ only exists to
         * satisfy ibv_qp_init_attr's requirement for one. */
        struct ibv_cq* cq;
        {
            PdLockGuard pdLock;
            cq = ibv_create_cq(deviceCtx_, 16, nullptr, nullptr, 0);
            if (!cq) {
                LOG_ERROR(
                    "rc-listener: rejecting CONNECT_REQUEST: ibv_create_cq "
                    "failed: %s",
                    std::strerror(errno));
                rdma_reject(id, nullptr, 0);
                rdma_destroy_id(id);
                return;
            }

            struct ibv_qp_init_attr initAttr;
            std::memset(&initAttr, 0, sizeof(initAttr));
            initAttr.send_cq = cq;
            initAttr.recv_cq = cq;
            initAttr.cap.max_send_wr = 16;
            initAttr.cap.max_recv_wr = 16;
            initAttr.cap.max_send_sge = 1;
            initAttr.cap.max_recv_sge = 1;
            initAttr.qp_type = IBV_QPT_RC;

            if (rdma_create_qp(id, pd_, &initAttr) != 0) {
                LOG_ERROR(
                    "rc-listener: rejecting CONNECT_REQUEST: rdma_create_qp "
                    "failed: %s",
                    std::strerror(errno));
                ibv_destroy_cq(cq);
                rdma_reject(id, nullptr, 0);
                rdma_destroy_id(id);
                return;
            }
        }

        struct rdma_conn_param param;
        std::memset(&param, 0, sizeof(param));
        /* Concurrent outstanding RDMA READs this side will service -- the
         * negotiated depth is min(initiator's request, responder's
         * capacity). */
        param.responder_resources = 16;
        param.initiator_depth = 16;
        param.rnr_retry_count = 7;

        if (rdma_accept(id, &param) != 0) {
            LOG_ERROR("rc-listener: rdma_accept failed: %s",
                      std::strerror(errno));
            DestroyConnection(id);
            return;
        }
        LOG_DEBUG(
            "rc-listener: accepted CONNECT_REQUEST on device %s, qp_num=0x%x",
            ibv_get_device_name(id->verbs->device), id->qp->qp_num);

        std::lock_guard<std::mutex> lock(mutex_);
        activeConns_.push_back(id);
    }

    void RdmaCmRcListener::HandleTeardown(struct rdma_cm_id* id) {
        RemoveActiveConn(id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (establishedCount_ > 0) {
                --establishedCount_;
            }
        }
        if (onDisconnected_) {
            onDisconnected_(userCtx_, id->qp);
        }
        rdma_disconnect(id); /* idempotent enough: ignore errors here */
        DestroyConnection(id);
    }

    /*!
     * A remote RDMA READ/WRITE against a registered MR never posts a work
     * request or generates a normal CQ completion on the target side --
     * RC transport services it entirely at the hardware/firmware level --
     * so a QP-level problem on an established (accepted) connection is
     * otherwise completely invisible to this passive listener: no
     * completion to poll, no CM event, nothing. The one place libibverbs
     * *does* surface a fatal QP/CQ/device condition is the context's
     * async event fd (`ibv_get_async_event`/`ibv_ack_async_event`) --
     * IBV_EVENT_QP_FATAL/QP_REQ_ERR/QP_ACCESS_ERR and friends. Logs
     * whatever comes through; does not attempt recovery (an accepted
     * connection reaching one of these states is already unusable, and
     * the caller's own token/reply-based error handling covers the
     * resulting request failure).
     *
     * Takes one event per call. ibv_get_async_event() blocks, so draining in
     * a loop would block after the last event, stalling the accept loop and
     * Stop()'s join(). The async fd is in AcceptLoop()'s epoll set
     * level-triggered, so epoll_wait() wakes the loop again while events
     * remain. The fd isn't made nonblocking instead: the context is
     * librdmacm's, not hipObject's.
     */
    void RdmaCmRcListener::PollAsyncEvents() {
        if (!deviceCtx_) {
            return;
        }
        struct ibv_async_event event;
        if (ibv_get_async_event(deviceCtx_, &event) == 0) {
            uint32_t qpNum = 0;
            switch (event.event_type) {
                case IBV_EVENT_QP_FATAL:
                case IBV_EVENT_QP_REQ_ERR:
                case IBV_EVENT_QP_ACCESS_ERR:
                case IBV_EVENT_PATH_MIG_ERR:
                case IBV_EVENT_QP_LAST_WQE_REACHED:
                    qpNum = event.element.qp ? event.element.qp->qp_num : 0;
                    break;
                default:
                    break;
            }
            LOG_ERROR(
                "rc-listener: async event %s (type=%d) qp_num=0x%x -- a "
                "silent RDMA READ/WRITE failure against an established "
                "connection surfaces here, since it generates no normal "
                "completion or CM event on this (target) side",
                ibv_event_type_str(event.event_type), event.event_type, qpNum);
            ibv_ack_async_event(&event);
        }
    }

    void RdmaCmRcListener::AcceptLoop() {
        int epollFd = epoll_create1(0);
        if (epollFd < 0) {
            LOG_ERROR("rc-listener: epoll_create1 failed: %s", std::strerror(errno));
            return;
        }
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = channel_->fd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, channel_->fd, &ev) != 0) {
            LOG_ERROR("rc-listener: epoll_ctl(ADD channel_->fd) failed: %s",
                      std::strerror(errno));
            close(epollFd);
            return;
        }

        // Level-triggered (no EPOLLET), which PollAsyncEvents() relies on
        if (deviceCtx_) {
            epoll_event asyncEv{};
            asyncEv.events = EPOLLIN;
            asyncEv.data.fd = deviceCtx_->async_fd;
            epoll_ctl(epollFd, EPOLL_CTL_ADD, deviceCtx_->async_fd, &asyncEv);
        }

        while (!stopping_.load()) {

            epoll_event events[2];
            int ready = epoll_wait(epollFd, events, 2,
                                   200 /* ms; wake periodically to check the stop flag */);
            if (ready <= 0) {
                continue;
            }

            bool channelReady = false;
            for (int i = 0; i < ready; ++i) {
                if (deviceCtx_ && events[i].data.fd == deviceCtx_->async_fd) {
                    PollAsyncEvents();
                } else if (events[i].data.fd == channel_->fd) {
                    channelReady = true;
                }
            }
            if (!channelReady) {
                continue;
            }

            struct rdma_cm_event* event = nullptr;
            if (rdma_get_cm_event(channel_, &event) != 0) {
                continue;
            }
            struct rdma_cm_id* id = event->id;
            enum rdma_cm_event_type type = event->event;
            rdma_ack_cm_event(event); /* frees `event`; `id` stays valid */

            switch (type) {
                case RDMA_CM_EVENT_CONNECT_REQUEST:
                    HandleConnectRequest(id);
                    break;
                case RDMA_CM_EVENT_ESTABLISHED: {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++establishedCount_;
                    LOG_DEBUG(
                        "rc-listener: ESTABLISHED qp_num=0x%x (establishedCount=%d)",
                        id->qp ? id->qp->qp_num : 0, establishedCount_);
                    if (onConnected_) {
                        onConnected_(userCtx_, id->qp);
                    }
                    break;
                }
                case RDMA_CM_EVENT_DISCONNECTED:
                case RDMA_CM_EVENT_CONNECT_ERROR:
                case RDMA_CM_EVENT_REJECTED:
                case RDMA_CM_EVENT_UNREACHABLE:
                case RDMA_CM_EVENT_DEVICE_REMOVAL:
                    LOG_DEBUG(
                        "rc-listener: teardown event %s qp_num=0x%x (is_listener=%d)",
                        rdma_event_str(type), id->qp ? id->qp->qp_num : 0,
                        id == listenId_);
                    if (id != listenId_) {
                        HandleTeardown(id);
                    }
                    break;
                default:
                    LOG_DEBUG("rc-listener: unhandled event %s",
                              rdma_event_str(type));
                    break;
            }
        }
        close(epollFd);
    }

    uint16_t rcListenerPortFromEnv() {
        const char* env = std::getenv("HIPOBJ_RC_LISTEN_PORT");
        if (env && env[0] != '\0') {
            char* end = nullptr;
            long val = std::strtol(env, &end, 10);
            if (end != env && *end == '\0' && val > 0 && val <= 65535) {
                return static_cast<uint16_t>(val);
            }
        }
        return kRcListenerDefaultPort;
    }

    RdmaCmRcListener* RdmaCmRcListener::Start(
        const char* bindIp, uint16_t port,
        RcListenerEventCallback onConnected,
        RcListenerEventCallback onDisconnected, void* userCtx) {
        /* A specific address: bound to INADDR_ANY, RDMA CM resolves no
         * device, so there'd be no context to allocate the PD on */
        if (!bindIp || bindIp[0] == '\0') {
            LOG_ERROR("rc-listener: Start() needs an address to bind to");
            return nullptr;
        }
        std::string key = bindIp;
        uint16_t resolvedPort = (port != 0) ? port : rcListenerPortFromEnv();
        LOG_INFO("rc-listener: Start() requested bindIp=%s port=%u",
                 key.c_str(), resolvedPort);

        std::lock_guard<std::mutex> registryLock(s_registryMutex);
        for (RdmaCmRcListener* existing : s_listeners) {
            if (existing->bindIp_ == key && existing->port_ == resolvedPort) {
                LOG_INFO(
                    "rc-listener: Start() found already-running listener for "
                    "%s:%u -- returning existing instance %p",
                    key.c_str(), resolvedPort, static_cast<void*>(existing));
                return existing; /* already running for this bindIp:port */
            }
        }

        /* librdmacm opens its own struct ibv_context for the device a bind
         * resolves to, and an accepted connection's id->verbs is always that
         * context. It's never pointer-equal to a context opened separately
         * with ibv_open_device(), even for the same device, and verbs objects
         * from one context can't be used through another. So the listener
         * takes the context from its own bind, allocates its PD on it, and
         * hands both back through deviceCtx()/pd(): hipObject registers its
         * buffers on that PD (registrationPd() in hipobj.cpp). */
        struct rdma_event_channel* channel = rdma_create_event_channel();
        if (!channel) {
            LOG_ERROR("rc-listener: rdma_create_event_channel failed: %s",
                      std::strerror(errno));
            return nullptr;
        }
        LOG_DEBUG("rc-listener: created CM event channel fd=%d", channel->fd);

        struct rdma_cm_id* listenId = nullptr;
        if (rdma_create_id(channel, &listenId, nullptr, RDMA_PS_TCP) != 0) {
            LOG_ERROR("rc-listener: rdma_create_id failed: %s",
                      std::strerror(errno));
            rdma_destroy_event_channel(channel);
            return nullptr;
        }

        /* key (bindIp) decides the family: an IPv6 literal always contains
         * ':' (dotted-quad IPv4 never does), so that's enough to pick
         * between sockaddr_in/sockaddr_in6. */
        sockaddr_storage addr{};
        if (key.find(':') != std::string::npos) {
            auto* addr6 = reinterpret_cast<sockaddr_in6*>(&addr);
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(resolvedPort);
            if (inet_pton(AF_INET6, key.c_str(), &addr6->sin6_addr) != 1) {
                LOG_ERROR("rc-listener: inet_pton failed for bindIp=\"%s\"",
                          key.c_str());
                rdma_destroy_id(listenId);
                rdma_destroy_event_channel(channel);
                return nullptr;
            }
        } else {
            auto* addr4 = reinterpret_cast<sockaddr_in*>(&addr);
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(resolvedPort);
            if (inet_pton(AF_INET, key.c_str(), &addr4->sin_addr) != 1) {
                LOG_ERROR("rc-listener: inet_pton failed for bindIp=\"%s\"",
                          key.c_str());
                rdma_destroy_id(listenId);
                rdma_destroy_event_channel(channel);
                return nullptr;
            }
        }
        if (rdma_bind_addr(listenId, reinterpret_cast<sockaddr*>(&addr)) != 0) {
            LOG_ERROR("rc-listener: rdma_bind_addr(%s:%u) failed: %s",
                      key.c_str(), resolvedPort, std::strerror(errno));
            rdma_destroy_id(listenId);
            rdma_destroy_event_channel(channel);
            return nullptr;
        }
        LOG_DEBUG("rc-listener: bound to %s:%u (device %s)",
                  key.c_str(), resolvedPort,
                  listenId->verbs
                      ? ibv_get_device_name(listenId->verbs->device)
                      : "(null verbs)");

        /* Was 16 -- raised to give slack against a burst of concurrent
         * initiator-side connection attempts (e.g. a server handling
         * many parallel PUT/GET calls against this one listener at
         * once, each needing its own fresh RDMA CM connection). A cheap,
         * independent safety margin against connection-establishment
         * timeouts under heavy concurrent load. */
        if (rdma_listen(listenId, 128) != 0) {
            LOG_ERROR("rc-listener: rdma_listen failed: %s",
                      std::strerror(errno));
            rdma_destroy_id(listenId);
            rdma_destroy_event_channel(channel);
            return nullptr;
        }
        LOG_DEBUG("rc-listener: listening (backlog=128)");

        if (!listenId->verbs) {
            LOG_ERROR(
                "rc-listener: listenId->verbs is null after bind -- no "
                "device resolved for %s", key.c_str());
            rdma_destroy_id(listenId);
            rdma_destroy_event_channel(channel);
            return nullptr;
        }
        struct ibv_context* resolvedCtx = listenId->verbs;
        struct ibv_pd* resolvedPd = nullptr;
        {
            PdLockGuard pdLock;
            resolvedPd = ibv_alloc_pd(resolvedCtx);
        }
        if (!resolvedPd) {
            LOG_ERROR("rc-listener: ibv_alloc_pd failed: %s",
                      std::strerror(errno));
            rdma_destroy_id(listenId);
            rdma_destroy_event_channel(channel);
            return nullptr;
        }
        LOG_DEBUG("rc-listener: PD allocated pd=%p on device %s",
                  static_cast<void*>(resolvedPd),
                  ibv_get_device_name(resolvedCtx->device));

        RdmaCmRcListener* listener = new RdmaCmRcListener();
        listener->bindIp_ = std::move(key);
        listener->channel_ = channel;
        listener->listenId_ = listenId;
        listener->port_ = resolvedPort;
        listener->pd_ = resolvedPd;
        listener->deviceCtx_ = resolvedCtx;
        listener->onConnected_ = onConnected;
        listener->onDisconnected_ = onDisconnected;
        listener->userCtx_ = userCtx;
        listener->stopping_.store(false);
        listener->thread_ = std::thread(&RdmaCmRcListener::AcceptLoop, listener);

        s_listeners.push_back(listener);
        LOG_INFO(
            "rc-listener: Start() succeeded listener=%p bindIp=%s port=%u "
            "pd=%p deviceCtx=%p (registry size=%zu)",
            static_cast<void*>(listener),
            listener->bindIp_.c_str(),
            listener->port_, static_cast<void*>(listener->pd_),
            static_cast<void*>(listener->deviceCtx_), s_listeners.size());
        return listener;
    }

    void RdmaCmRcListener::Stop(RdmaCmRcListener* listener) {
        if (!listener) {
            LOG_DEBUG("rc-listener: Stop(nullptr) -- no-op");
            return;
        }
        LOG_INFO("rc-listener: Stop() requested for listener=%p port=%u",
                 static_cast<void*>(listener), listener->port());
        {
            std::lock_guard<std::mutex> registryLock(s_registryMutex);
            auto it = std::find(s_listeners.begin(), s_listeners.end(), listener);
            if (it == s_listeners.end()) {
                LOG_WARN(
                    "rc-listener: Stop() called for listener=%p not found in "
                    "registry -- already stopped or never started via Start()",
                    static_cast<void*>(listener));
                return; /* unknown / already stopped */
            }
            s_listeners.erase(it);
        }
        LOG_DEBUG("rc-listener: removed listener=%p from registry",
                  static_cast<void*>(listener));

        /* Removed from the registry (so no new Start() call can find it)
         * before touching thread_/activeConns_ below, and the registry
         * lock is released before join(): AcceptLoop() never blocks on
         * s_registryMutex, but join() must never be called while holding
         * a lock the loop might need to make progress toward exiting. */
        listener->stopping_.store(true);
        if (listener->thread_.joinable()) {
            listener->thread_.join();
            LOG_DEBUG("rc-listener: accept-loop thread joined for listener=%p",
                      static_cast<void*>(listener));
        }

        {
            std::lock_guard<std::mutex> lock(listener->mutex_);
            LOG_DEBUG(
                "rc-listener: tearing down %zu active connection(s) for "
                "listener=%p",
                listener->activeConns_.size(), static_cast<void*>(listener));
            for (struct rdma_cm_id* id : listener->activeConns_) {
                rdma_disconnect(id);
                listener->DestroyConnection(id);
            }
            listener->activeConns_.clear();
            listener->establishedCount_ = 0;
        }
        if (listener->pd_) {
            PdLockGuard pdLock;
            if (ibv_dealloc_pd(listener->pd_) != 0) {
                LOG_ERROR("rc-listener: ibv_dealloc_pd failed for listener=%p",
                          static_cast<void*>(listener));
            }
        }
        rdma_destroy_id(listener->listenId_);
        rdma_destroy_event_channel(listener->channel_);
        LOG_INFO("rc-listener: Stop() complete for listener=%p",
                 static_cast<void*>(listener));
        delete listener;
    }

    uint16_t RdmaCmRcListener::port() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return port_;
    }

    int RdmaCmRcListener::connectionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return establishedCount_;
    }

    void* RdmaCmRcListener::deviceCtx() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceCtx_;
    }

    void* RdmaCmRcListener::pd() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pd_;
    }

    std::string RdmaCmRcListener::deviceName() const {
        std::scoped_lock<std::mutex> lock(mutex_);
        const char* name =
            deviceCtx_ ? ibv_get_device_name(deviceCtx_->device) : nullptr;
        return name ? std::string(name) : std::string();
    }

    void RdmaCmRcListener::stop() {
        Stop(this);
    }

} // namespace hipObj
