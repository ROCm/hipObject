/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "buffer.h"
#include "hipobj.h"
#include "transport.h"

namespace hipObj {

class HipRuntime;
class NicTopology;
class RcListener;
class Verbs;

/* Stops an RC-CM listener, which is how one is released */
struct RcListenerDeleter {
    void operator()(RcListener *listener) const noexcept;
};

using RcListenerPtr = std::unique_ptr<RcListener, RcListenerDeleter>;

/* The outside world the library calls: the HIP runtime, libibverbs, and
 * the NIC topology. It refers to them, and doesn't own them, so whatever
 * owns it has to keep them alive for as long as it lives. */
class Platform final {
public:
    Platform(const HipRuntime &hip, const Verbs &verbs, const NicTopology &topology) noexcept
        : hip_(&hip), verbs_(&verbs), topology_(&topology)
    {
    }

    const HipRuntime &hip() const noexcept
    {
        return *hip_;
    }

    const Verbs &verbs() const noexcept
    {
        return *verbs_;
    }

    const NicTopology &topology() const noexcept
    {
        return *topology_;
    }

private:
    const HipRuntime  *hip_;
    const Verbs       *verbs_;
    const NicTopology *topology_;
};

/* A platform over hip(), ibv(), and the NIC topology NIC selection uses.
 * The returned pointer owns the SysfsTopology it uses when no test
 * installed a topology. Call it only once ibvAvailable() has returned
 * true. */
std::shared_ptr<const Platform> systemPlatform();

/* The settings that hipObjInit() reads from the environment, and how long
 * a transfer waits for its completion */
struct Options {
    /* How long to wait for a staging copy before giving up
     * (HIPOBJ_STAGE_TIMEOUT_MS, a whole number of milliseconds up to 24
     * hours). A 1 MiB copy is single-digit milliseconds on real hardware
     * and ~170 ms on the emulated GPU the CI lanes use, so the default is
     * several orders of magnitude of slack. It exists for one reason: a
     * DMA that never completes must not become an unkillable process.
     * ROCm waits on the completion signal with
     * BusyWaitSignal::WaitRelaxed, which spins in userspace rather than
     * blocking, so a wedged copy shows up as a busy core and no kernel log
     * at all -- there is no other layer that will ever time this out. */
    std::chrono::milliseconds stageTimeout = std::chrono::seconds(30);

    /* How long a transfer waits for its work completion. It isn't read
     * from the environment. */
    std::chrono::milliseconds pollTimeout = std::chrono::seconds(5);

    /* Strict GPU-direct mode (HIPOBJ_REQUIRE_GPU_DIRECT). Registering a
     * device buffer is supposed to hand the NIC the device memory itself;
     * when that fails we silently fall back to a host staging buffer and
     * copy through it, which still transfers the right bytes and so passes
     * every assertion a test can make -- a GPU-direct lane that quietly
     * stopped being GPU-direct reports success. CI sets this so the
     * fallback is a hard failure there, while a production host without
     * dmabuf keeps working. */
    bool requireGpuDirect = false;

    /* HIPOBJ_DMABUF and HIPOBJ_RDMA_DEBUG */
    DmabufPolicy dmabuf{};
};

/* Looks up an environment variable by name: its value, or null when it
 * isn't set */
using EnvLookup = std::function<const char *(const char *name)>;

/* The options the environment variables that lookup finds set. A
 * variable that isn't set, or whose value isn't valid, leaves the
 * default. */
Options parseOptions(const EnvLookup &lookup);

/* The library's state from hipObjInit() to hipObjShutdown(). Members are
 * destroyed in reverse order: the buffers are deregistered before the
 * connection's protection domain is deallocated, and the platform outlives
 * both. */
struct Instance final {
    Instance(std::shared_ptr<const Platform> instancePlatform, const Options &instanceOptions) noexcept;
    /* Deregisters the buffers, then stops the RC-CM listener, before the
     * members are destroyed: the buffers' MRs can be on the listener's own
     * PD, which Stop() deallocates, and the connection is closed after the
     * listener's QPs on its PD are gone */
    ~Instance();

    /* Lives in a shared_ptr, and isn't copied or moved */
    Instance(const Instance &)            = delete;
    Instance &operator=(const Instance &) = delete;
    Instance(Instance &&)                 = delete;
    Instance &operator=(Instance &&)      = delete;

    std::shared_ptr<const Platform> platform{};
    Options                         options{};

    /* From the hipObjConfig_t, with gpuDevice resolved and the NIC
     * chosen */
    int         gpuDevice = 0;
    std::string endpoint{};
    std::string region{};
    std::string nicHint{};
    int         nicIndex = -1;
    uint32_t    flags    = 0;
    /* v3: the address the RC-CM listener binds to and tokens advertise,
     * HIPOBJ_RC_LISTEN_IP or the chosen NIC's. Worked out once, in
     * initWith(). */
    std::string listenIp{};

    // hipObject only ever manages one RC-CM listener at a time (one per
    // Instance, see initWith()) even though RdmaCmRcListener supports
    // several concurrently -- this is that one instance's handle, from
    // startRcListener(). In v3, hipObjInit() only succeeds with it running,
    // and buffers register on its PD (registrationPd() in hipobj.cpp).
    RcListenerPtr listener{};

    /* v1's device, PD and QP. In v3 it's only opened to choose the NIC and
     * read its GID, and isn't opened at all when HIPOBJ_RC_LISTEN_IP is
     * set. */
    RcConnection conn{};
    BufferMap    buffers;
};

/* The library's instance, or null when it isn't initialized. Call it with
 * the API lock held (see ApiGuard), or while no other thread can call the
 * library. */
std::shared_ptr<Instance> currentInstance();

// Selects the RDMA control-plane wire format hipObjGet()/hipObjPut()/
// hipObjGetRdmaToken() use: v3 (default -- RC-CM dial-back, see
// v3-wire.h) or v1 (QP number/GID/LID exchanged directly, see
// token.h), for interop with peers that only understand the older
// format. Fixed at compile time via -DHIPOBJ_RDMA_WIRE_V1 (CMakeLists.txt's
// HIPOBJ_RDMA_WIRE_V1 option): hipobj is built for one wire format or the
// other, not switchable at runtime.
constexpr bool
useV1Wire()
{
#ifdef HIPOBJ_RDMA_WIRE_V1
    return true;
#else
    return false;
#endif
}

// The IPv4 address a GID encodes (RoCEv2 IPv4-mapped GIDs only), or empty
// when it has none. initWith() binds the RC-CM listener to the chosen
// NIC's, unless HIPOBJ_RC_LISTEN_IP names the address peers should dial
// instead, for a NIC whose GID has none or when this picks the wrong
// interface. Mirrors parseClientNicFromTokenHex()'s convention (token.cpp).
std::string localNicIp(const ibv_gid &localGid);

/* Starts hipObject's RC-CM listener (v3) on bindIp and port (0 for
 * rcListenerPortFromEnv()): RdmaCmRcListener::Start(), or the function a
 * test installed with setRcListenerStartForTest(). Null when it can't
 * start. */
RcListenerPtr startRcListener(const char *bindIp, uint16_t port);

/* A function that starts an RC-CM listener, as startRcListener() does */
using RcListenerStart = RcListener *(*)(const char *bindIp, uint16_t port);

/* Makes startRcListener() call start, or RdmaCmRcListener::Start() when
 * it's null. Returns the function installed before. */
RcListenerStart setRcListenerStartForTest(RcListenerStart start);

/* hipObjInit(), with its platform and options given: builds an Instance
 * that holds them, and makes it the current one. The caller holds the API
 * lock, and has checked config as hipObjInit() does. Returns
 * hipObjAlreadyInitialized if there's already an instance. Throws only
 * before anything changes.
 *
 * Only the GPU lookup calls the platform's HIP runtime. NIC selection, the
 * RDMA device, buffer registration, transfers, and the owning pointers'
 * deleters call hip(), ibv(), and the installed NIC topology, so platform
 * has to be one that systemPlatform() returned while the same ones were
 * installed. */
hipObjError_t initWith(const hipObjConfig_t &config, std::shared_ptr<const Platform> platform,
                       const Options &options);

/* Destroys the current instance, if there is one, releasing what it holds.
 * The caller holds the API lock. */
void shutdownInstance() noexcept;

/* Holds the library-wide API lock for as long as it lives. Every public
 * function documented as serialized (see the "threads" group in
 * hipobj.h) takes it on its first line and holds it for the whole call,
 * so the library runs one such call at a time. That includes every
 * function that reads or changes library state, which is the current
 * Instance and everything it holds.
 *
 * hipObjGet() and hipObjPut() call the application's callbacks with the
 * lock held, and the lock isn't recursive, so a callback that called a
 * locked function would deadlock. The guard detects that instead: when
 * the calling thread already holds the lock, it doesn't lock, and owns()
 * returns false. */
class ApiGuard final {
public:
    ApiGuard();
    ~ApiGuard();
    ApiGuard(const ApiGuard &)            = delete;
    ApiGuard &operator=(const ApiGuard &) = delete;
    ApiGuard(ApiGuard &&)                 = delete;
    ApiGuard &operator=(ApiGuard &&)      = delete;

    /* False when the calling thread already held the lock */
    [[nodiscard]] bool owns() const noexcept;

private:
    std::unique_lock<std::mutex> lock_;
};

} // namespace hipObj
