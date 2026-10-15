/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "abandoned-copies.h"
#include "buffer.h"
#include "hipobj.h"
#include "transport.h"

namespace hipObj {

class HipRuntime;
class NicTopology;
class Verbs;

/* The outside world the library calls: the HIP runtime, libibverbs, and
 * the NIC topology. It refers to them, and doesn't own them, so whatever
 * owns it has to keep them alive for as long as it lives. */
class Platform final {
public:
    Platform(const HipRuntime &hip, const Verbs &verbs, const NicTopology &topology) noexcept
        : hip_(&hip), verbs_(&verbs), topology_(&topology)
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

private:
    const HipRuntime  *hip_;
    const Verbs       *verbs_;
    const NicTopology *topology_;
};

/* The platform over the system's HIP runtime, libibverbs, and the NIC
 * topology sysfs reports, or null when libibverbs can't be loaded. Every
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

/* What hipObjInit() chose from the hipObjConfig_t: the GPU, with -1
 * resolved, and the NIC */
struct ResolvedConfig {
    int         gpuDevice = 0;
    std::string nicHint{};
    int         nicIndex = -1;
};

/* The library's state from hipObjInit() to hipObjShutdown(). It's
 * complete when it's built, and only the connection's QP state, the
 * buffers, and the abandoned staging copies change after that. The buffer
 * map and the abandoned copies have locks of their own; the connection is
 * only used by transfers, which hold the API lock (see ApiGuard).
 *
 * It lives in a shared_ptr, and each call that uses it holds a copy (see
 * currentInstance()), so it's destroyed when hipObjShutdown() has removed
 * it and the last of those calls has returned. Members are destroyed in
 * reverse order: the abandoned copies that are over are released, and the
 * buffers deregistered, and the connection's QP and CQ destroyed, before
 * the device's protection domain is deallocated, and the platform
 * outlives them all. */
struct Instance final {
    /* instanceDevice has to have come from instancePlatform's verbs, and
     * instanceConn has to have been created on *instanceDevice. The device
     * is on the heap so the connection can be created on it, at the
     * address it keeps, before the instance is built. */
    Instance(std::shared_ptr<const Platform> instancePlatform, const Options &instanceOptions,
             ResolvedConfig instanceConfig, std::shared_ptr<const RdmaDevice> instanceDevice,
             RcConnection instanceConn) noexcept;
    ~Instance() = default;

    /* Lives in a shared_ptr, and isn't copied or moved */
    Instance(const Instance &)            = delete;
    Instance &operator=(const Instance &) = delete;
    Instance(Instance &&)                 = delete;
    Instance &operator=(Instance &&)      = delete;

    std::shared_ptr<const Platform>   platform;
    Options                           options;
    ResolvedConfig                    config;
    std::shared_ptr<const RdmaDevice> device;
    RcConnection                      conn;
    BufferMap                         buffers;
    AbandonedCopies                   abandoned;
};

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
 * library uninitialized. The instance, and what it holds, is released
 * here if nothing else holds it, or else when the last call that holds it
 * (see currentInstance()) lets it go. */
void shutdownInstance();

/* Holds the library-wide API lock for as long as it lives. Every public
 * function documented as serialized (see the "threads" group in
 * hipobj.h) takes it on its first line and holds it for the whole call,
 * so the library runs one such call at a time. That includes every
 * function that reads or changes library state, which is the current
 * Instance and everything it holds. The current instance and the buffer
 * map have locks of their own, but the instance's connection relies on
 * this one.
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
