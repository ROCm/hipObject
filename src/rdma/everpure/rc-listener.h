/* SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ibv-core.h"
#include "rdmacm-core.h"
#include "thread-safety.h"

/*
 * RDMA CM listener for hipobj-rc-v3.
 *
 * A hipobj-rc-v3 peer reads the listener's address
 * out of the advertised token (see v3-wire.h) and calls
 * rdma_connect() at it; RDMA CM negotiates the QP-level connection
 * setup on both ends, so hipObject never has to exchange QP number,
 * GID or LID by hand the way v1 does.
 *
 * The listener calls librdmacm through an RdmaCm and libibverbs through
 * a Verbs, the platform's (see Platform in state.h), so the unit tests
 * run it on fakes. It uses rdmacm-core.h's and ibv-core.h's types, the
 * same ones those interfaces take.
 */

namespace hipObj {

class RdmaCm;
class Verbs;

/*! @brief Default TCP port for the RC-CM listener, overridable via
 *         the HIPOBJ_RC_LISTEN_PORT environment variable (see
 *         Options::rcListenPort in state.h). */
constexpr uint16_t kRcListenerDefaultPort = 18181;

/*!
 * Invoked from a listener's background thread. `qp` is the
 * struct ibv_qp* RDMA CM created for the connection.
 */
using RcListenerEventCallback = void (*)(void *userCtx, void *qp);

/*!
 * One RDMA CM listener bound to a specific IP:port.
 *
 * Instances are never constructed directly -- go through the static
 * Start()/Stop() pair. Each Start() starts a listener of its own, so
 * a second one on an IP:port that's in use fails to bind. Sharing one
 * between instances is a TODO (startRcListener() in state.cpp).
 */
class RdmaCmRcListener final {
public:
    RdmaCmRcListener(const RdmaCmRcListener &)            = delete;
    RdmaCmRcListener &operator=(const RdmaCmRcListener &) = delete;
    RdmaCmRcListener(RdmaCmRcListener &&)                 = delete;
    RdmaCmRcListener &operator=(RdmaCmRcListener &&)      = delete;
    ~RdmaCmRcListener();

    /*!
     * Starts an RDMA CM listener bound to `bindIp`:`port`, calling
     * librdmacm through `cm` and libibverbs through `verbs`, which have
     * to outlive it. `bindIp` is required: it's the address peers dial
     * (see initWith() in state.cpp), and binding to it is how RDMA CM
     * picks the device.
     *
     * The listener takes its device context from that bind -- the
     * context librdmacm opened, which every accepted connection arrives
     * on -- and allocates its own PD there. Every accepted connection
     * gets a QP on that PD, so register the buffers peers access on it
     * too (see deviceCtx()/pd()).
     *
     * `port` is HIPOBJ_RC_LISTEN_PORT's (Options::rcListenPort).
     * `onConnected` / `onDisconnected` may be null.
     *
     * @return the running listener, which the caller releases with
     * Stop(), or nullptr on failure (no address, or a bind, listen or
     * PD allocation error).
     */
    static RdmaCmRcListener *Start(const RdmaCm &cm, const Verbs &verbs, const char *bindIp, uint16_t port,
                                   RcListenerEventCallback onConnected,
                                   RcListenerEventCallback onDisconnected, void *userCtx);

    /*!
     * Stops `listener`'s thread, disconnects and destroys any live
     * connections, frees its PD and releases its CM event channel.
     * `listener` (and any raw pointer to it) is invalid once this
     * returns. Safe to call with nullptr (no-op).
     */
    static void Stop(RdmaCmRcListener *listener) HIPOBJ_REQUIRES(!listener->mutex_);

    /*! @return the TCP port this listener is bound to. */
    uint16_t port() const;

    /*! @return the number of peer connections currently ESTABLISHED. */
    int connectionCount() const HIPOBJ_REQUIRES(!mutex_);

    /*!
     * @return the struct ibv_context* the bind resolved to (librdmacm's,
     * see Start())
     */
    ibv_context *deviceCtx() const;

    /*!
     * @return the struct ibv_pd* the listener allocated on that
     * context: the one to register buffers on
     */
    ibv_pd *pd() const;

    /*! @return the libibverbs name of the device the bind resolved to */
    std::string deviceName() const;

private:
    RdmaCmRcListener(const RdmaCm &cm, const Verbs &verbs) : cm_(&cm), verbs_(&verbs)
    {
    }

    void AcceptLoop() HIPOBJ_REQUIRES(!mutex_);
    void PollAsyncEvents();
    void HandleConnectRequest(rdma_cm_id *id) HIPOBJ_REQUIRES(!mutex_);
    void HandleTeardown(rdma_cm_id *id) HIPOBJ_REQUIRES(!mutex_);
    void DestroyConnection(rdma_cm_id *id);
    void RemoveActiveConn(rdma_cm_id *id) HIPOBJ_REQUIRES(!mutex_);

    const RdmaCm *cm_;
    const Verbs  *verbs_;
    mutable Mutex mutex_{};
    /* Set by Start() before the accept thread starts, and not changed
     * until Stop() has joined it, so they're read without mutex_ */
    std::string             bindIp_{};
    rdma_event_channel     *channel_        = nullptr;
    rdma_cm_id             *listenId_       = nullptr;
    uint16_t                port_           = 0;
    ibv_pd                 *pd_             = nullptr;
    ibv_context            *deviceCtx_      = nullptr;
    RcListenerEventCallback onConnected_    = nullptr;
    RcListenerEventCallback onDisconnected_ = nullptr;
    void                   *userCtx_        = nullptr;
    /* The accept thread, which only Start() and Stop() touch */
    std::thread thread_{};
    /* Set by Stop() while the accept thread runs and reads it */
    std::atomic<bool> stopping_{false};
    /* Accepted (non-listening) ids */
    std::vector<rdma_cm_id *> activeConns_ HIPOBJ_GUARDED_BY(mutex_){};
    int                       establishedCount_ HIPOBJ_GUARDED_BY(mutex_) = 0;
};

/*! Stops a listener, which is how one is released */
struct RcListenerDeleter {
    void operator()(RdmaCmRcListener *listener) const noexcept
    {
        RdmaCmRcListener::Stop(listener);
    }
};

/*! A running listener, from RdmaCmRcListener::Start() */
using RcListenerPtr = std::unique_ptr<RdmaCmRcListener, RcListenerDeleter>;

} // namespace hipObj
