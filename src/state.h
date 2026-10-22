/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "abandoned-copies.h"
#include "buffer.h"
#include "connection-pool.h"
#include "hipobj.h"
#include "rc-listener.h"
#include "transport.h"

namespace hipObj {

class HipRuntime;
class NicTopology;
class RdmaCm;
class Verbs;

/* The outside world the library calls: the HIP runtime, libibverbs, the
 * NIC topology, and librdmacm (null in a v1 build, which doesn't use it).
 * It refers to them, and doesn't own them, so whatever owns it has to keep
 * them alive for as long as it lives. */
class Platform final {
public:
    Platform(const HipRuntime &hip, const Verbs &verbs, const NicTopology &topology,
             const RdmaCm *rdmaCm) noexcept
        : hip_(&hip), verbs_(&verbs), topology_(&topology), rdmaCm_(rdmaCm)
    {
    }

    [[nodiscard]] const HipRuntime &hip() const noexcept
    {
        return *hip_;
    }

    [[nodiscard]] const Verbs &verbs() const noexcept
    {
        return *verbs_;
    }

    [[nodiscard]] const NicTopology &topology() const noexcept
    {
        return *topology_;
    }

    /* The RC-CM listener's (v3), or null in a v1 build */
    [[nodiscard]] const RdmaCm *rdmaCm() const noexcept
    {
        return rdmaCm_;
    }

private:
    const HipRuntime  *hip_;
    const Verbs       *verbs_;
    const NicTopology *topology_;
    const RdmaCm      *rdmaCm_;
};

/* The platform over the system's HIP runtime, libibverbs, the NIC
 * topology sysfs reports, and, in a v3 build, librdmacm, or null when
 * libibverbs can't be loaded. Every
 * call returns the same platform, built on the first call, which loads
 * libibverbs. It's never destroyed, so libibverbs stays loaded until the
 * process exits. */
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
    static constexpr std::chrono::seconds kDefaultStageTimeout{30};
    std::chrono::milliseconds             stageTimeout = kDefaultStageTimeout;

    /* How long a transfer waits for its work completion. It isn't read
     * from the environment. */
    static constexpr std::chrono::seconds kDefaultPollTimeout{5};
    std::chrono::milliseconds             pollTimeout = kDefaultPollTimeout;

    /* How many connections hipObjInit() creates for transfers
     * (HIPOBJ_MAX_TRANSFERS, from 1 to 1024), and so how many transfers
     * can run at once. Each one is a QP and a CQ on the NIC. */
    static constexpr size_t kDefaultMaxTransfers = 16;
    size_t                  maxTransfers         = kDefaultMaxTransfers;

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

    /* The address the RC-CM listener binds to and the v3 token advertises
     * (HIPOBJ_RC_LISTEN_IP), or empty for the chosen NIC's. v3 only: see
     * initWith(). */
    std::string rcListenIp{};

    /* The RC-CM listener's TCP port (HIPOBJ_RC_LISTEN_PORT, from 1 to
     * 65535). v3 only. */
    uint16_t rcListenPort = kRcListenerDefaultPort;
};

/* Looks up an environment variable by name: its value, or null when it
 * isn't set */
using EnvLookup = std::function<const char *(const char *name)>;

/* The options the environment variables that lookup finds set. A
 * variable that isn't set, or whose value isn't valid, leaves the
 * default. */
Options parseOptions(const EnvLookup &lookup);

/* What hipObjInit() chose from the hipObjConfig_t: the GPU, with -1
 * resolved, and the NIC */
struct ResolvedConfig {
    int         gpuDevice = 0;
    std::string nicHint{};
    int         nicIndex = -1;
};

/* The library's state from hipObjInit() to hipObjShutdown(). It's
 * complete when it's built, and only the transfers' connections, the
 * buffers, and the abandoned staging copies change after that, and each
 * of those has a lock of its own. The token connection never changes. A
 * v3 build has no QP of its own (the RC-CM listener creates one for each
 * connection it accepts), so its pool is empty and it has no token
 * connection.
 *
 * It lives in a shared_ptr, and each call that uses it holds a copy (see
 * currentInstance()), so it's destroyed when hipObjShutdown() has removed
 * it and the last of those calls has returned. Members are destroyed in
 * reverse order: the abandoned copies that are over are released, the
 * buffers deregistered, and the connections' QPs and CQs destroyed, before
 * the RC-CM listener is stopped, which deallocates its protection domain,
 * and before the device's is, and the platform outlives them all. */
struct Instance final {
    /* instanceDevice has to have come from instancePlatform's verbs, and
     * instanceTokenConn has to have been created on *instanceDevice. The
     * device is on the heap so the connection can be created on it, at
     * the address it keeps, before the instance is built. Creates
     * instanceOptions.maxTransfers connections in the pool in v1, or fewer
     * if they can't all be created (see ConnectionPool), and none in v3.
     * instanceListener, which is null in v1, has to have been started on
     * instancePlatform's librdmacm and verbs, and bound to
     * instanceListenIp. */
    Instance(std::shared_ptr<const Platform> instancePlatform, Options instanceOptions,
             ResolvedConfig instanceConfig, std::shared_ptr<const RdmaDevice> instanceDevice,
             std::optional<RcConnection> instanceTokenConn, RcListenerPtr instanceListener,
             std::string instanceListenIp);
    ~Instance() = default;

    /* Lives in a shared_ptr, and isn't copied or moved */
    Instance(const Instance &)            = delete;
    Instance &operator=(const Instance &) = delete;
    Instance(Instance &&)                 = delete;
    Instance &operator=(Instance &&)      = delete;

    std::shared_ptr<const Platform> platform;
    Options                         options;
    ResolvedConfig                  config;
    /* v3: the address the RC-CM listener binds to and tokens advertise,
     * HIPOBJ_RC_LISTEN_IP or the chosen NIC's. Worked out once, in
     * initWith(). */
    std::string listenIp;

    /* In v1, the device the connections are on. In v3 the device only
     * gives the NIC's GID, and with HIPOBJ_RC_LISTEN_IP it's the NIC the
     * listener's bind resolved to. */
    std::shared_ptr<const RdmaDevice> device;
    /* v3: the RC-CM listener, which hipObjInit() only succeeds with.
     * Buffers register on its PD (registrationPd() in hipobj.cpp), so it's
     * declared before the buffers, and stopped after they're
     * deregistered. */
    RcListenerPtr listener;
    /* The connection whose QP number hipObjGetRdmaToken() advertises in
     * v1. It stays in INIT, since nothing tells hipObject when a transfer
     * that the application drives itself ends, so it's never leased or
     * connected. */
    std::optional<RcConnection> tokenConn;
    ConnectionPool              pool;
    BufferMap                   buffers;
    AbandonedCopies             abandoned;
};

// Selects the RDMA control-plane wire format hipObjGet()/hipObjPut()/
// hipObjGetRdmaToken() use: v1 (default -- QP number/GID/LID exchanged
// directly, see token.h) or v3 (RC-CM dial-back, see v3-wire.h). Fixed
// at compile time via -DHIPOBJ_RDMA_WIRE_V3 (CMakeLists.txt's
// HIPOBJ_RDMA_WIRE_V3 option): hipobj is built for one wire format or the
// other, not switchable at runtime.
constexpr bool
useV1Wire()
{
#ifndef HIPOBJ_RDMA_WIRE_V3
    return true;
#else
    return false;
#endif
}

// The IPv4 address a GID encodes (RoCEv2 IPv4-mapped GIDs only), or empty
// when it has none. initWith() binds the RC-CM listener to the chosen
// NIC's, unless HIPOBJ_RC_LISTEN_IP (Options::rcListenIp) names the
// address peers should dial instead, for a NIC whose GID has none or when
// this picks the wrong interface. Mirrors parseClientNicFromTokenHex()'s
// convention (token.cpp).
std::string localNicIp(const ibv_gid &localGid);

/* The library's instance, or null when it isn't initialized. The copy
 * keeps the instance alive for as long as the caller holds it, even if
 * another thread shuts the library down in the meantime. */
std::shared_ptr<Instance> currentInstance();

/* hipObjInit(), with its platform and options given: builds an Instance
 * that holds them, and makes it the current one. The caller has checked
 * config as hipObjInit() does. Returns hipObjAlreadyInitialized if there's
 * already an instance. Throws only before anything changes.
 *
 * It runs one at a time with shutdownInstance(), so of several calls at
 * once, only one opens a device and succeeds.
 *
 * Everything the instance does, from choosing the NIC to transfers, calls
 * the outside world through platform. */
hipObjError_t initWith(const hipObjConfig_t &config, std::shared_ptr<const Platform> platform,
                       const Options &options);

/* Removes the current instance, if there is one, so later calls find the
 * library uninitialized, and closes its connection pool, so calls that
 * hold it can't start a transfer. The instance, and what it holds, is
 * released here if nothing else holds it, or else when the last call that
 * holds it (see currentInstance()) lets it go. */
void shutdownInstance();

} // namespace hipObj
