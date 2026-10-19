/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <hip/hip_runtime_api.h>

#include "connection-pool.h"
#include "hip-seam.h"
#include "hipobj-private.h"
#include "hipobj-parse.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "no-destructor.h"
#include "rdma-topology.h"
#include "thread-safety.h"

namespace hipObj {

namespace {

    /* The current instance, and the locks that guard it. They're taken in
     * the order they're declared, which the thread safety analysis
     * checks. */
    struct Current {
        /* Held for the whole of initWith() and shutdownInstance(), so they
         * run one at a time. Calls that only read the instance never take
         * it. */
        Mutex initMutex{};
        /* Guards instance. Calls that read it take it shared, so they
         * don't wait for each other, and only initWith() and
         * shutdownInstance(), which change it, take it exclusively. */
        SharedMutex               mutex HIPOBJ_ACQUIRED_AFTER(initMutex){};
        std::shared_ptr<Instance> instance HIPOBJ_GUARDED_BY(mutex){};
    };

    /* Never destroyed: destroying an instance releases verbs and HIP
     * resources, which isn't safe during static destruction, so
     * hipObjShutdown() is what releases it. SharedMutex has a
     * destructor, so it's never destroyed either. */
    Current &current()
    {
        static NoDestructor<Current> state;
        return *state;
    }

    /* The longest staging copy timeout: longer than any copy could take,
     * and short enough that adding it to the current time can't overflow
     * the clock */
    constexpr std::chrono::milliseconds kMaxStageTimeout = std::chrono::hours(24);

    /* The most connections hipObjInit() creates for transfers. Far more
     * transfers than a NIC can keep busy, and few enough that creating
     * the connections doesn't make initialization slow. */
    constexpr size_t kMaxTransfers = 1024;

    /* The value of environment variable name, or an empty string when it
     * isn't set */
    std::string_view lookupValue(const EnvLookup &lookup, const char *name)
    {
        const char *value = lookup(name);
        return value != nullptr ? std::string_view(value) : std::string_view();
    }

    /* Whether value starts with one of chars */
    bool startsWithOneOf(std::string_view value, std::string_view chars)
    {
        return !value.empty() && chars.find(value.front()) != std::string_view::npos;
    }

    /* The platform systemPlatform() returns, and the interfaces it refers
     * to. The members are built in the order they're declared, so verbs_
     * is set before topology_ and platform_ refer to it. */
    class SystemPlatform final {
    public:
        explicit SystemPlatform(std::unique_ptr<LoadedVerbs> verbs) noexcept
            : verbs_(std::move(verbs)), topology_(*verbs_), platform_(hip_, *verbs_, topology_)
        {
        }

        [[nodiscard]] const Platform &platform() const noexcept
        {
            return platform_;
        }

    private:
        SystemHip                    hip_{};
        std::unique_ptr<LoadedVerbs> verbs_;
        SysfsTopology                topology_;
        Platform                     platform_;
    };

    /* A new system platform, or null when libibverbs can't be loaded */
    std::shared_ptr<const Platform> makeSystemPlatform()
    {
        std::unique_ptr<LoadedVerbs> verbs = loadLibibverbs();
        if (!verbs) {
            return nullptr;
        }

        /* Points to the platform, and shares ownership of the
         * SystemPlatform that holds it */
        const auto system = std::make_shared<const SystemPlatform>(std::move(verbs));
        return {system, &system->platform()};
    }

} // namespace

std::shared_ptr<const Platform>
systemPlatform()
{
    /* Built on the first call, not when the library loads, so an
     * application that never initializes hipObject never loads
     * libibverbs. Never destroyed, so it outlives an instance that's never
     * shut down. */
    static NoDestructor<std::shared_ptr<const Platform>> platform(makeSystemPlatform());
    return *platform;
}

Options
parseOptions(const EnvLookup &lookup)
{
    Options options;

    /* A value that isn't a whole number of milliseconds in range is
     * ignored */
    if (const auto ms = parseNumber<int64_t>(lookupValue(lookup, "HIPOBJ_STAGE_TIMEOUT_MS"))) {
        if (*ms > 0 && *ms <= kMaxStageTimeout.count()) {
            options.stageTimeout = std::chrono::milliseconds(*ms);
        }
    }

    /* So is a value that isn't a whole number from 1 to kMaxTransfers */
    if (const auto n = parseNumber<size_t>(lookupValue(lookup, "HIPOBJ_MAX_TRANSFERS"))) {
        if (*n > 0 && *n <= kMaxTransfers) {
            options.maxTransfers = *n;
        }
    }

    const std::string_view requireGpuDirect = lookupValue(lookup, "HIPOBJ_REQUIRE_GPU_DIRECT");
    options.requireGpuDirect = !requireGpuDirect.empty() && !startsWithOneOf(requireGpuDirect, "0nN");

    options.dmabuf.enabled = !startsWithOneOf(lookupValue(lookup, "HIPOBJ_DMABUF"), "0nN");

    const std::string_view debug = lookupValue(lookup, "HIPOBJ_RDMA_DEBUG");
    options.dmabuf.debug         = !debug.empty() && !debug.starts_with('0');
    return options;
}

/* The members are initialized in the order they're declared, so the
 * pool and the buffers use the members, not the arguments moved into
 * them */
Instance::Instance(std::shared_ptr<const Platform> instancePlatform, const Options &instanceOptions,
                   ResolvedConfig instanceConfig, std::shared_ptr<const RdmaDevice> instanceDevice,
                   RcConnection instanceTokenConn)
    : platform(std::move(instancePlatform)), options(instanceOptions), config(std::move(instanceConfig)),
      device(std::move(instanceDevice)), tokenConn(std::move(instanceTokenConn)),
      pool(device, options.maxTransfers),
      buffers(platform->verbs(), platform->hip(), options.dmabuf, options.requireGpuDirect),
      abandoned(platform->hip())
{
}

std::shared_ptr<Instance>
currentInstance()
{
    Current         &state = current();
    const SharedLock lock(state.mutex);
    return state.instance;
}

void
shutdownInstance()
{
    Current                &state = current();
    const ScopedLock<Mutex> initLock(state.initMutex);

    /* Moving the instance out leaves state.instance null. Calls that
     * already hold a copy keep the instance alive. */
    std::shared_ptr<Instance> instance;
    {
        const ScopedLock<SharedMutex> lock(state.mutex);
        instance = std::move(state.instance);
    }

    /* Releases the instance if nothing else holds it, without holding
     * the lock that calls take to find it. Calls that hold it, and wait
     * for a connection, stop waiting. */
    if (instance) {
        instance->pool.close();
    }
    instance.reset();
}

hipObjError_t
initWith(const hipObjConfig_t &config, std::shared_ptr<const Platform> platform, const Options &options)
{
    Current                &state = current();
    const ScopedLock<Mutex> initLock(state.initMutex);
    if (currentInstance()) {
        return makeError(hipObjAlreadyInitialized);
    }
    const HipRuntime      &runtime = platform->hip();
    const std::string_view nicHint =
        config.nicHint != nullptr ? std::string_view(config.nicHint) : std::string_view();
    const bool haveNicHint = !nicHint.empty();
    int        gpuDevice   = config.gpuDevice;
    if (gpuDevice < 0) {
        const hipError_t err = runtime.getDevice(&gpuDevice);
        if (err != hipSuccess) {
            // No GPU to infer a device from. That is only fatal when we also have
            // no NIC hint -- with a hint the topology lookup below is skipped
            // entirely, so an absent GPU is not an error.
            if (err != hipErrorNoDevice || !haveNicHint) {
                return makeError(hipObjRdmaError, static_cast<int>(err));
            }
            gpuDevice = -1;
        }
    }
    else {
        // An explicit device has to exist, even when a NIC hint means its
        // topology isn't needed
        int              deviceCount = 0;
        const hipError_t err         = runtime.getDeviceCount(&deviceCount);
        if (err == hipErrorNoDevice) {
            deviceCount = 0;
        }
        else if (err != hipSuccess) {
            return makeError(hipObjRdmaError, static_cast<int>(err));
        }
        if (gpuDevice >= deviceCount) {
            return makeError(hipObjInvalidValue);
        }
    }
    std::optional<RdmaNic> nic;
    if (gpuDevice >= 0) {
        nic = GetClosestNicToGpu(runtime, platform->topology(), gpuDevice, nicHint);
    }
    if (!nic) {
        // GPU topology lookup failed (no GPU or no matching NIC). When a NIC name
        // hint is provided, try opening it directly without GPU topology so the
        // library works in GPU-less environments (e.g. CI with emulated RDMA).
        if (haveNicHint) {
            nic = RdmaNic{.index = 0, .name = std::string(nicHint)};
        }
        else {
            return makeError(hipObjNicNotFound);
        }
    }

    /* The device, the token connection, and then the instance and its
     * pool are built in locals, which release what they hold if anything
     * fails or throws. The instance is only published once nothing else
     * can. */
    std::optional<RdmaDevice> opened =
        openRdmaDeviceByName(platform->verbs(), platform->topology(), nic->name);
    if (!opened) {
        return makeError(hipObjRdmaError);
    }
    auto                        device    = std::make_shared<const RdmaDevice>(std::move(*opened));
    std::optional<RcConnection> tokenConn = createRcConnection(
        *device, ConnectionPool::kCqSize, ConnectionPool::kMaxSendWr, ConnectionPool::kMaxRecvWr);
    if (!tokenConn) {
        return makeError(hipObjRdmaError);
    }
    ResolvedConfig resolved{
        .gpuDevice = gpuDevice,
        .nicHint   = std::string(nicHint),
        .nicIndex  = nic->index,
    };
    auto instance = std::make_shared<Instance>(std::move(platform), options, std::move(resolved),
                                               std::move(device), std::move(*tokenConn));
    /* The pool stops at the first connection it can't create */
    if (instance->pool.size() != options.maxTransfers) {
        return makeError(hipObjRdmaError);
    }

    /* If taking the lock throws, the instance is released, and nothing
     * has changed. Moving the instance in can't throw. */
    const ScopedLock<SharedMutex> lock(state.mutex);
    state.instance = std::move(instance);
    return makeError(hipObjSuccess);
}

} // namespace hipObj
