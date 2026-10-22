/* SPDX-License-Identifier: MIT
 */

#include "rc-listener.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hipobj-sockaddr.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "log.h"
#include "rdmacm-core.h"
#include "rdmacm-wrapper.h"
#include "thread-safety.h"

namespace hipObj {

namespace {

    /* Each connection's CQ, and its QP's send and receive queues.
     * hipObject posts no work requests of its own on them. */
    constexpr int kConnQueueDepth = 16;
    /* Concurrent outstanding RDMA READs this side will service, and
     * asks to initiate -- the negotiated depth is min(initiator's
     * request, responder's capacity) */
    constexpr uint8_t kRdmaReadDepth = 16;
    /* RNR retries; 7 means retry indefinitely */
    constexpr uint8_t kRnrRetryCount = 7;
    /* How long the accept loop waits for an event before checking the
     * stop flag */
    constexpr int kAcceptLoopWakeMs = 200;
    /* rdma_listen()'s backlog */
    constexpr int kListenBacklog = 128;

} // namespace

RdmaCmRcListener::~RdmaCmRcListener() = default;

void
RdmaCmRcListener::DestroyConnection(rdma_cm_id *id)
{
    if (id->qp != nullptr) {
        ibv_cq *sendCq = id->qp->send_cq;
        cm_->destroy_qp(id);
        if (sendCq != nullptr) {
            (void)verbs_->destroy_cq(sendCq);
        }
    }
    (void)cm_->destroy_id(id);
}

void
RdmaCmRcListener::RemoveActiveConn(rdma_cm_id *id)
{
    const ScopedLock<Mutex> lock(mutex_);
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
void
RdmaCmRcListener::HandleConnectRequest(rdma_cm_id *id)
{
    /* Start() took deviceCtx_ from the listen id, so every connection
     * RDMA CM accepts on it should arrive on the same context. Kept as a
     * guard: a QP, CQ or MR from one context can't be used through
     * another, even on the same device. */
    if (id->verbs != deviceCtx_) {
        logError("rc-listener: rejecting CONNECT_REQUEST: it arrived on {}:{}, "
                 "but the listener's PD and CQs are on {}:{}",
                 id->verbs != nullptr ? verbs_->get_device_name(id->verbs->device) : "(null)",
                 static_cast<void *>(id->verbs),
                 deviceCtx_ != nullptr ? verbs_->get_device_name(deviceCtx_->device) : "(null)",
                 static_cast<void *>(deviceCtx_));
        (void)cm_->reject(id, nullptr, 0);
        (void)cm_->destroy_id(id);
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
    ibv_cq *cq = verbs_->create_cq(deviceCtx_, kConnQueueDepth, nullptr, nullptr, 0);
    if (cq == nullptr) {
        logError("rc-listener: rejecting CONNECT_REQUEST: ibv_create_cq "
                 "failed: {}",
                 std::strerror(errno));
        (void)cm_->reject(id, nullptr, 0);
        (void)cm_->destroy_id(id);
        return;
    }

    ibv_qp_init_attr initAttr{};
    initAttr.send_cq          = cq;
    initAttr.recv_cq          = cq;
    initAttr.cap.max_send_wr  = kConnQueueDepth;
    initAttr.cap.max_recv_wr  = kConnQueueDepth;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;
    initAttr.qp_type          = IBV_QPT_RC;

    if (cm_->create_qp(id, pd_, &initAttr) != 0) {
        logError("rc-listener: rejecting CONNECT_REQUEST: rdma_create_qp "
                 "failed: {}",
                 std::strerror(errno));
        (void)verbs_->destroy_cq(cq);
        (void)cm_->reject(id, nullptr, 0);
        (void)cm_->destroy_id(id);
        return;
    }

    rdma_conn_param param{};
    param.responder_resources = kRdmaReadDepth;
    param.initiator_depth     = kRdmaReadDepth;
    param.rnr_retry_count     = kRnrRetryCount;

    if (cm_->accept(id, &param) != 0) {
        logError("rc-listener: rdma_accept failed: {}", std::strerror(errno));
        DestroyConnection(id);
        return;
    }
    logDebug("rc-listener: accepted CONNECT_REQUEST on device {}, qp_num=0x{:x}",
             verbs_->get_device_name(id->verbs->device), id->qp->qp_num);

    const ScopedLock<Mutex> lock(mutex_);
    activeConns_.push_back(id);
}

void
RdmaCmRcListener::HandleTeardown(rdma_cm_id *id)
{
    RemoveActiveConn(id);
    {
        const ScopedLock<Mutex> lock(mutex_);
        if (establishedCount_ > 0) {
            --establishedCount_;
        }
    }
    if (onDisconnected_ != nullptr) {
        onDisconnected_(userCtx_, id->qp);
    }
    (void)cm_->disconnect(id); /* idempotent enough: ignore errors here */
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
void
RdmaCmRcListener::PollAsyncEvents()
{
    if (deviceCtx_ == nullptr) {
        return;
    }
    ibv_async_event event{};
    if (verbs_->get_async_event(deviceCtx_, &event) == 0) {
        uint32_t qpNum = 0;
        switch (event.event_type) {
            case IBV_EVENT_QP_FATAL:
            case IBV_EVENT_QP_REQ_ERR:
            case IBV_EVENT_QP_ACCESS_ERR:
            case IBV_EVENT_PATH_MIG_ERR:
            case IBV_EVENT_QP_LAST_WQE_REACHED: {
                /* libibverbs reports the element in a union, whose
                 * member the event type selects: these are QP events */
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
                const ibv_qp *qp = event.element.qp;
                qpNum            = qp != nullptr ? qp->qp_num : 0;
                break;
            }
            case IBV_EVENT_CQ_ERR:
            case IBV_EVENT_COMM_EST:
            case IBV_EVENT_SQ_DRAINED:
            case IBV_EVENT_PATH_MIG:
            case IBV_EVENT_DEVICE_FATAL:
            case IBV_EVENT_PORT_ACTIVE:
            case IBV_EVENT_PORT_ERR:
            case IBV_EVENT_LID_CHANGE:
            case IBV_EVENT_PKEY_CHANGE:
            case IBV_EVENT_SM_CHANGE:
            case IBV_EVENT_SRQ_ERR:
            case IBV_EVENT_SRQ_LIMIT_REACHED:
            case IBV_EVENT_CLIENT_REREGISTER:
            case IBV_EVENT_GID_CHANGE:
            case IBV_EVENT_WQ_FATAL:
            default:
                break;
        }
        logError("rc-listener: async event {} (type={}) qp_num=0x{:x} -- a "
                 "silent RDMA READ/WRITE failure against an established "
                 "connection surfaces here, since it generates no normal "
                 "completion or CM event on this (target) side",
                 verbs_->event_type_str(event.event_type), static_cast<int>(event.event_type), qpNum);
        verbs_->ack_async_event(&event);
    }
}

void
RdmaCmRcListener::AcceptLoop()
{
    const int epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd < 0) {
        logError("rc-listener: epoll_create1 failed: {}", std::strerror(errno));
        return;
    }
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = channel_->fd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, channel_->fd, &ev) != 0) {
        logError("rc-listener: epoll_ctl(ADD channel_->fd) failed: {}", std::strerror(errno));
        close(epollFd);
        return;
    }

    // Level-triggered (no EPOLLET), which PollAsyncEvents() relies on
    if (deviceCtx_ != nullptr) {
        epoll_event asyncEv{};
        asyncEv.events  = EPOLLIN;
        asyncEv.data.fd = deviceCtx_->async_fd;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, deviceCtx_->async_fd, &asyncEv);
    }

    while (!stopping_.load()) {

        std::array<epoll_event, 2> events{};
        const int                  ready =
            epoll_wait(epollFd, events.data(), static_cast<int>(events.size()), kAcceptLoopWakeMs);
        if (ready <= 0) {
            continue;
        }

        bool channelReady = false;
        for (const epoll_event &event : std::span(events).first(static_cast<size_t>(ready))) {
            if (deviceCtx_ != nullptr && event.data.fd == deviceCtx_->async_fd) {
                PollAsyncEvents();
            }
            else if (event.data.fd == channel_->fd) {
                channelReady = true;
            }
        }
        if (!channelReady) {
            continue;
        }

        rdma_cm_event *event = nullptr;
        if (cm_->get_cm_event(channel_, &event) != 0) {
            continue;
        }
        rdma_cm_id              *id   = event->id;
        const rdma_cm_event_type type = event->event;
        (void)cm_->ack_cm_event(event); /* frees `event`; `id` stays valid */

        switch (type) {
            case RDMA_CM_EVENT_CONNECT_REQUEST:
                HandleConnectRequest(id);
                break;
            case RDMA_CM_EVENT_ESTABLISHED: {
                int established = 0;
                {
                    const ScopedLock<Mutex> lock(mutex_);
                    established = ++establishedCount_;
                }
                logDebug("rc-listener: ESTABLISHED qp_num=0x{:x} (establishedCount={})",
                         id->qp != nullptr ? id->qp->qp_num : 0, established);
                if (onConnected_ != nullptr) {
                    onConnected_(userCtx_, id->qp);
                }
                break;
            }
            case RDMA_CM_EVENT_DISCONNECTED:
            case RDMA_CM_EVENT_CONNECT_ERROR:
            case RDMA_CM_EVENT_REJECTED:
            case RDMA_CM_EVENT_UNREACHABLE:
            case RDMA_CM_EVENT_DEVICE_REMOVAL:
                logDebug("rc-listener: teardown event {} qp_num=0x{:x} (is_listener={:d})",
                         cm_->event_str(type), id->qp != nullptr ? id->qp->qp_num : 0, id == listenId_);
                if (id != listenId_) {
                    HandleTeardown(id);
                }
                break;
            case RDMA_CM_EVENT_ADDR_RESOLVED:
            case RDMA_CM_EVENT_ADDR_ERROR:
            case RDMA_CM_EVENT_ROUTE_RESOLVED:
            case RDMA_CM_EVENT_ROUTE_ERROR:
            case RDMA_CM_EVENT_CONNECT_RESPONSE:
            case RDMA_CM_EVENT_MULTICAST_JOIN:
            case RDMA_CM_EVENT_MULTICAST_ERROR:
            case RDMA_CM_EVENT_ADDR_CHANGE:
            case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            default:
                logDebug("rc-listener: unhandled event {}", cm_->event_str(type));
                break;
        }
    }
    close(epollFd);
}

RdmaCmRcListener *
RdmaCmRcListener::Start(const RdmaCm &cm, const Verbs &verbs, const char *bindIp, uint16_t port,
                        RcListenerEventCallback onConnected, RcListenerEventCallback onDisconnected,
                        void *userCtx)
{
    /* A specific address: bound to INADDR_ANY, RDMA CM resolves no
     * device, so there'd be no context to allocate the PD on */
    if (bindIp == nullptr || *bindIp == '\0') {
        logError("rc-listener: Start() needs an address to bind to");
        return nullptr;
    }
    std::string key = bindIp;
    logInfo("rc-listener: Start() requested bindIp={} port={}", key.c_str(), port);

    /* librdmacm opens its own struct ibv_context for the device a bind
     * resolves to, and an accepted connection's id->verbs is always that
     * context. It's never pointer-equal to a context opened separately
     * with ibv_open_device(), even for the same device, and verbs objects
     * from one context can't be used through another. So the listener
     * takes the context from its own bind, allocates its PD on it, and
     * hands both back through deviceCtx()/pd(): hipObject registers its
     * buffers on that PD (registrationPd() in hipobj.cpp). */
    rdma_event_channel *channel = cm.create_event_channel();
    if (channel == nullptr) {
        logError("rc-listener: rdma_create_event_channel failed: {}", std::strerror(errno));
        return nullptr;
    }
    logDebug("rc-listener: created CM event channel fd={}", channel->fd);

    rdma_cm_id *listenId = nullptr;
    if (cm.create_id(channel, &listenId, nullptr, RDMA_PS_TCP) != 0) {
        logError("rc-listener: rdma_create_id failed: {}", std::strerror(errno));
        cm.destroy_event_channel(channel);
        return nullptr;
    }

    /* key (bindIp) decides the family: an IPv6 literal always contains
     * ':' (dotted-quad IPv4 never does), so that's enough to pick
     * between sockaddr_in/sockaddr_in6. */
    sockaddr_in6 addr6{};
    sockaddr_in  addr4{};
    sockaddr    *addr = nullptr;
    if (key.find(':') != std::string::npos) {
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port   = htons(port);
        addr              = asSockaddr(addr6);
        if (inet_pton(AF_INET6, key.c_str(), &addr6.sin6_addr) != 1) {
            logError("rc-listener: inet_pton failed for bindIp=\"{}\"", key.c_str());
            (void)cm.destroy_id(listenId);
            cm.destroy_event_channel(channel);
            return nullptr;
        }
    }
    else {
        addr4.sin_family = AF_INET;
        addr4.sin_port   = htons(port);
        addr             = asSockaddr(addr4);
        if (inet_pton(AF_INET, key.c_str(), &addr4.sin_addr) != 1) {
            logError("rc-listener: inet_pton failed for bindIp=\"{}\"", key.c_str());
            (void)cm.destroy_id(listenId);
            cm.destroy_event_channel(channel);
            return nullptr;
        }
    }
    if (cm.bind_addr(listenId, addr) != 0) {
        logError("rc-listener: rdma_bind_addr({}:{}) failed: {}", key.c_str(), port, std::strerror(errno));
        (void)cm.destroy_id(listenId);
        cm.destroy_event_channel(channel);
        return nullptr;
    }
    logDebug("rc-listener: bound to {}:{} (device {})", key.c_str(), port,
             listenId->verbs != nullptr ? verbs.get_device_name(listenId->verbs->device) : "(null verbs)");

    /* Was 16 -- raised to give slack against a burst of concurrent
     * initiator-side connection attempts (e.g. a server handling
     * many parallel PUT/GET calls against this one listener at
     * once, each needing its own fresh RDMA CM connection). A cheap,
     * independent safety margin against connection-establishment
     * timeouts under heavy concurrent load. */
    if (cm.listen(listenId, kListenBacklog) != 0) {
        logError("rc-listener: rdma_listen failed: {}", std::strerror(errno));
        (void)cm.destroy_id(listenId);
        cm.destroy_event_channel(channel);
        return nullptr;
    }
    logDebug("rc-listener: listening (backlog={})", kListenBacklog);

    if (listenId->verbs == nullptr) {
        logError("rc-listener: listenId->verbs is null after bind -- no "
                 "device resolved for {}",
                 key.c_str());
        (void)cm.destroy_id(listenId);
        cm.destroy_event_channel(channel);
        return nullptr;
    }
    ibv_context *resolvedCtx = listenId->verbs;
    ibv_pd      *resolvedPd  = verbs.alloc_pd(resolvedCtx);
    if (resolvedPd == nullptr) {
        logError("rc-listener: ibv_alloc_pd failed: {}", std::strerror(errno));
        (void)cm.destroy_id(listenId);
        cm.destroy_event_channel(channel);
        return nullptr;
    }
    logDebug("rc-listener: PD allocated pd={} on device {}", static_cast<void *>(resolvedPd),
             verbs.get_device_name(resolvedCtx->device));

    /* The constructor is private, so std::make_unique() can't call it */
    std::unique_ptr<RdmaCmRcListener> listener(new RdmaCmRcListener(cm, verbs));
    listener->bindIp_         = std::move(key);
    listener->channel_        = channel;
    listener->listenId_       = listenId;
    listener->port_           = port;
    listener->pd_             = resolvedPd;
    listener->deviceCtx_      = resolvedCtx;
    listener->onConnected_    = onConnected;
    listener->onDisconnected_ = onDisconnected;
    listener->userCtx_        = userCtx;
    listener->stopping_.store(false);
    listener->thread_ = std::thread(&RdmaCmRcListener::AcceptLoop, listener.get());

    logInfo("rc-listener: Start() succeeded listener={} bindIp={} port={} "
            "pd={} deviceCtx={}",
            static_cast<void *>(listener.get()), listener->bindIp_.c_str(), listener->port_,
            static_cast<void *>(listener->pd_), static_cast<void *>(listener->deviceCtx_));
    return listener.release();
}

void
RdmaCmRcListener::Stop(RdmaCmRcListener *listener)
{
    if (listener == nullptr) {
        logDebug("rc-listener: Stop(nullptr) -- no-op");
        return;
    }
    /* Deletes the listener once it's stopped, on the way out */
    const std::unique_ptr<RdmaCmRcListener> owned(listener);
    logInfo("rc-listener: Stop() requested for listener={} port={}", static_cast<void *>(listener),
            listener->port());

    /* join() must never be called while holding a lock the loop might
     * need to make progress toward exiting. */
    listener->stopping_.store(true);
    if (listener->thread_.joinable()) {
        listener->thread_.join();
        logDebug("rc-listener: accept-loop thread joined for listener={}", static_cast<void *>(listener));
    }

    /* The accept thread is gone, so the connections are only this
     * thread's: take them out, and release the lock before the librdmacm
     * and libibverbs calls */
    std::vector<rdma_cm_id *> conns;
    {
        const ScopedLock<Mutex> lock(listener->mutex_);
        conns.swap(listener->activeConns_);
        listener->establishedCount_ = 0;
    }
    logDebug("rc-listener: tearing down {} active connection(s) for "
             "listener={}",
             conns.size(), static_cast<void *>(listener));
    for (rdma_cm_id *id : conns) {
        (void)listener->cm_->disconnect(id);
        listener->DestroyConnection(id);
    }
    if (listener->pd_ != nullptr && listener->verbs_->dealloc_pd(listener->pd_) != 0) {
        logError("rc-listener: ibv_dealloc_pd failed for listener={}", static_cast<void *>(listener));
    }
    (void)listener->cm_->destroy_id(listener->listenId_);
    listener->cm_->destroy_event_channel(listener->channel_);
    logInfo("rc-listener: Stop() complete for listener={}", static_cast<void *>(listener));
}

uint16_t
RdmaCmRcListener::port() const
{
    return port_;
}

int
RdmaCmRcListener::connectionCount() const
{
    const ScopedLock<Mutex> lock(mutex_);
    return establishedCount_;
}

ibv_context *
RdmaCmRcListener::deviceCtx() const
{
    return deviceCtx_;
}

ibv_pd *
RdmaCmRcListener::pd() const
{
    return pd_;
}

std::string
RdmaCmRcListener::deviceName() const
{
    const char *name = deviceCtx_ != nullptr ? verbs_->get_device_name(deviceCtx_->device) : nullptr;
    return name != nullptr ? std::string(name) : std::string();
}

} // namespace hipObj
