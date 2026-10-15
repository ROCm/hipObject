/* SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
 * RDMA CM listener for hipobj-rc-v3.
 *
 * A hipobj-rc-v3 peer reads the listener's address
 * out of the advertised token (see v3-wire.h) and calls
 * rdma_connect() at it; RDMA CM negotiates the QP-level connection
 * setup on both ends, so hipObject never has to exchange QP number,
 * GID or LID by hand the way v1 does.
 *
 * This header only forward-declares the librdmacm/libibverbs struct
 * *names* it needs for RdmaCmRcListener's own member pointers (never their
 * bodies -- no ibv-core.h, no transport.h): rc-listener.cpp links the
 * real librdmacm/libibverbs headers directly (a real link dependency
 * for anything that pulls this file in -- see CMakeLists.txt), which
 * would collide with the vendored, dlopen-oriented struct definitions
 * in ibv-core.h if both were visible in one translation unit. Callers
 * (hipobj.cpp) still pass their own PD/device context through as
 * void* in the public API; on the vendored side those are
 * ABI-identical to the real "struct ibv_pd *" / "struct ibv_context *"
 * that rc-listener.cpp casts them to -- both ultimately describe the
 * one real libibverbs.so loaded into the process, whether reached via
 * hipObject's dlopen wrapper or via this file's direct link.
 */

struct rdma_event_channel;
struct rdma_cm_id;
struct ibv_pd;
struct ibv_context;

namespace hipObj {

    /*! @brief Default TCP port for the RC-CM listener, overridable via
     *         the HIPOBJ_RC_LISTEN_PORT environment variable. */
    constexpr uint16_t kRcListenerDefaultPort = 18181;

    /*!
     * Resolves the RC-CM listener port from the HIPOBJ_RC_LISTEN_PORT
     * environment variable, falling back to kRcListenerDefaultPort when
     * the variable is unset or does not parse as a value in [1, 65535].
     */
    uint16_t rcListenerPortFromEnv();

    /*!
     * Invoked from a listener's background thread. `qp` is the real
     * struct ibv_qp* RDMA CM created for the connection (see the
     * ABI note above).
     */
    using RcListenerEventCallback = void (*)(void* userCtx, void* qp);

    /*!
     * Interface for a running RC-CM listener, as hipObject uses it: an
     * RdmaCmRcListener, or a fake in the unit tests (see startRcListener()
     * in state.h).
     */
    class RcListener {
    public:
        RcListener() = default;
        virtual ~RcListener() = default;
        RcListener(const RcListener&) = delete;
        RcListener& operator=(const RcListener&) = delete;
        RcListener(RcListener&&) = delete;
        RcListener& operator=(RcListener&&) = delete;

        /*! @return the TCP port this listener is bound to. */
        virtual uint16_t port() const = 0;

        /*! @return the number of peer connections currently ESTABLISHED. */
        virtual int connectionCount() const = 0;

        /*!
         * @return the struct ibv_pd* to register buffers on, as a void*:
         * accepted connections' QPs are on it
         */
        virtual void* pd() const = 0;

        /*! @return the name of the device the bind resolved to, such as
         *  "mlx5_0" */
        virtual std::string deviceName() const = 0;

        /*! Stops the listener. It mustn't be used afterwards. */
        virtual void stop() = 0;
    };

    /*!
     * One RDMA CM listener bound to a specific IP:port.
     *
     * Instances are never constructed directly -- go through the static
     * Start()/Stop() pair. Every currently-running instance lives in a
     * static registry guarded by a static lock (see Start()/Stop()), so
     * the process can run more than one listener at once (distinct
     * IP:port pairs); Start() is idempotent per IP:port, returning the
     * already-running instance for that pair rather than starting a
     * second one.
     */
    class RdmaCmRcListener final : public RcListener {
    public:
        RdmaCmRcListener(const RdmaCmRcListener&) = delete;
        RdmaCmRcListener& operator=(const RdmaCmRcListener&) = delete;
        ~RdmaCmRcListener() override;

        /*!
         * Starts (if not already running for this bindIp:port) an RDMA CM
         * listener bound to `bindIp`:<port>. `bindIp` is required: it's the
         * address peers dial (see initWith() in state.cpp), and binding to
         * it is how RDMA CM picks the device.
         *
         * The listener takes its device context from that bind -- the
         * context librdmacm opened, which every accepted connection arrives
         * on -- and allocates its own PD there. Every accepted connection
         * gets a QP on that PD, so register the buffers peers access on it
         * too (see deviceCtx()/pd()).
         *
         * `port` == 0 resolves via rcListenerPortFromEnv(). `onConnected` /
         * `onDisconnected` may be null. Idempotent per bindIp:port: calling
         * this again for a pair that is already running returns that same
         * instance untouched (new callback arguments are ignored in that
         * case).
         *
         * @return a non-owning pointer to the running listener (freshly
         * started, or already running for this bindIp:port) on success, or
         * nullptr on failure (no address, or a bind, listen or PD
         * allocation error). The pointer remains valid until a matching
         * Stop() call.
         */
        static RdmaCmRcListener* Start(
            const char* bindIp, uint16_t port,
            RcListenerEventCallback onConnected,
            RcListenerEventCallback onDisconnected, void* userCtx);

        /*!
         * Stops `listener`'s thread, disconnects and destroys any live
         * connections, releases its CM event channel, and removes it from
         * the static registry. `listener` (and any raw pointer to it) is
         * invalid once this returns. Safe to call with nullptr (no-op).
         */
        static void Stop(RdmaCmRcListener* listener);

        /*! Stop(this) */
        void stop() override;

        /*! @return the TCP port this listener is bound to. */
        uint16_t port() const override;

        /*! @return the number of peer connections currently ESTABLISHED. */
        int connectionCount() const override;

        /*!
         * @return the struct ibv_context* the bind resolved to (librdmacm's,
         * see Start()), as a void*
         */
        void* deviceCtx() const;

        /*!
         * @return the struct ibv_pd* the listener allocated on that context,
         * as a void*: the one to register buffers on
         */
        void* pd() const override;

        /*! @return the name of the device the bind resolved to, such as
         *  "mlx5_0" */
        std::string deviceName() const override;

    private:
        RdmaCmRcListener() = default;

        void AcceptLoop();
        void PollAsyncEvents();
        void HandleConnectRequest(struct rdma_cm_id* id);
        void HandleTeardown(struct rdma_cm_id* id);
        void DestroyConnection(struct rdma_cm_id* id);
        void RemoveActiveConn(struct rdma_cm_id* id);

        mutable std::mutex mutex_;
        std::string bindIp_;
        struct rdma_event_channel* channel_ = nullptr;
        struct rdma_cm_id* listenId_ = nullptr;
        uint16_t port_ = 0;
        struct ibv_pd* pd_ = nullptr;
        struct ibv_context* deviceCtx_ = nullptr;
        RcListenerEventCallback onConnected_ = nullptr;
        RcListenerEventCallback onDisconnected_ = nullptr;
        void* userCtx_ = nullptr;
        std::thread thread_;
        std::atomic<bool> stopping_{false};
        /* Accepted (non-listening) ids, guarded by mutex_. */
        std::vector<struct rdma_cm_id*> activeConns_;
        int establishedCount_ = 0;

        /* Every currently-running listener, guarded by s_registryMutex. */
        static std::mutex s_registryMutex;
        static std::vector<RdmaCmRcListener*> s_listeners;
    };

} // namespace hipObj
