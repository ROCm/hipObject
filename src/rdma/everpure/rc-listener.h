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
 * *names* it needs for RcListener's own member pointers (never their
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
     * One RDMA CM listener bound to a specific IP:port (or any-IP,
     * INADDR_ANY, when bindIp is null/empty).
     *
     * Instances are never constructed directly -- go through the static
     * Start()/Stop() pair. Every currently-running instance lives in a
     * static registry guarded by a static lock (see Start()/Stop()), so
     * the process can run more than one listener at once (distinct
     * IP:port pairs); Start() is idempotent per IP:port, returning the
     * already-running instance for that pair rather than starting a
     * second one.
     */
    class RcListener {
    public:
        RcListener(const RcListener&) = delete;
        RcListener& operator=(const RcListener&) = delete;
        ~RcListener();

        /*!
         * Starts (if not already running for this bindIp:port) an RDMA CM
         * listener bound to `bindIp`:<port> (INADDR_ANY when `bindIp` is
         * null/empty). `pd`/`deviceCtx` must be the real "struct ibv_pd *"
         * and "struct ibv_context *" hipObject already opened (see
         * transport.h's openRdmaDevice) -- an accepted connection gets a QP
         * created on `pd`, so RDMA operations against MRs registered there
         * (hipObjBufRegister) can be serviced by it. A CONNECT_REQUEST whose
         * device does not match `deviceCtx` is rejected rather than accepted
         * against the wrong PD -- callers should therefore pass the address
         * they advertise to peers (see hipobj.cpp's localNicIp()) as
         * `bindIp` and open `pd`/`deviceCtx` from the same address (see
         * transport.h's openRdmaDeviceForIp()), so binding and the device
         * check agree on which physical device services the connection.
         *
         * `port` == 0 resolves via rcListenerPortFromEnv(). `onConnected` /
         * `onDisconnected` may be null. Idempotent per bindIp:port: calling
         * this again for a pair that is already running returns that same
         * instance untouched (new pd/deviceCtx/callback arguments are
         * ignored in that case).
         *
         * Passing `pd == deviceCtx == nullptr` selects self-managed mode:
         * the listener allocates its own PD from whatever device its bind
         * resolves to (the only context an accepted connection's QP can
         * ever validly use -- see the .cpp for why a caller-opened context
         * never matches). Retrieve them afterwards with deviceCtx()/pd() to
         * register buffers against the same device, e.g. for
         * HIPOBJ_RC_LISTEN_IP.
         *
         * @return a non-owning pointer to the running listener (freshly
         * started, or already running for this bindIp:port) on success, or
         * nullptr on failure (e.g. bind/listen error). The pointer remains
         * valid until a matching Stop() call.
         */
        static RcListener* Start(
            void* pd, void* deviceCtx, const char* bindIp, uint16_t port,
            RcListenerEventCallback onConnected,
            RcListenerEventCallback onDisconnected, void* userCtx);

        /*!
         * Stops `listener`'s thread, disconnects and destroys any live
         * connections, releases its CM event channel, and removes it from
         * the static registry. `listener` (and any raw pointer to it) is
         * invalid once this returns. Safe to call with nullptr (no-op).
         */
        static void Stop(RcListener* listener);

        /*! @return the TCP port this listener is bound to. */
        uint16_t port() const;

        /*! @return the number of peer connections currently ESTABLISHED. */
        int connectionCount() const;

        /*!
         * @return the self-managed struct ibv_context* (see Start()'s
         * self-managed mode) as a void*, or nullptr when the caller supplied
         * its own pd/deviceCtx to Start() instead.
         */
        void* deviceCtx() const;

        /*!
         * @return the self-managed struct ibv_pd* (see Start()'s
         * self-managed mode) as a void*, or nullptr when the caller supplied
         * its own pd/deviceCtx to Start() instead.
         */
        void* pd() const;

    private:
        RcListener() = default;

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
        /* True once deviceCtx_ is confirmed real (self-managed mode, or
         * a matched CONNECT_REQUEST) -- gates AcceptLoop()'s async_fd
         * polling against an unverified/bogus caller-provided pointer. */
        bool deviceCtxVerified_ = false;
        /* True when Start() allocated pd_ itself (self-managed mode, see
         * Start()'s doc comment) rather than being handed one by the
         * caller; only then does Stop() free it. */
        bool ownsPd_ = false;
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
        static std::vector<RcListener*> s_listeners;
    };

} // namespace hipObj
