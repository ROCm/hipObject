/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "hipobj-parse.h"
#include "hipobj-warnings.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "no-destructor.h"
#include "rdma-topology.h"

namespace hipObj {

// True while the calling thread holds the API lock. Each thread has its
// own copy, so it needs no synchronization.
static thread_local bool t_holdsApiLock = false;

static std::mutex &
apiMutex()
{
    static std::mutex lock;
    return lock;
}

ApiGuard::ApiGuard() : lock_(apiMutex(), std::defer_lock)
{
    if (t_holdsApiLock) {
        return;
    }
    lock_.lock();
    t_holdsApiLock = true;
}

// lock_ is destroyed, and the mutex unlocked, after this body runs
ApiGuard::~ApiGuard()
{
    if (lock_.owns_lock()) {
        t_holdsApiLock = false;
    }
}

bool
ApiGuard::owns() const noexcept
{
    return lock_.owns_lock();
}

namespace {

    /* The current instance, which the API lock guards. Never destroyed:
     * destroying an instance releases verbs and HIP resources, which isn't
     * safe during static destruction, so hipObjShutdown() is what releases
     * it. Clang still warns, since NoDestructor's empty destructor isn't
     * trivial. */
    std::shared_ptr<Instance> &current()
    {
        HIPOBJ_WARN_NO_EXIT_DTOR_OFF
        static NoDestructor<std::shared_ptr<Instance>> instance;
        HIPOBJ_WARN_NO_EXIT_DTOR_ON
        return *instance;
    }

    /* The longest staging copy timeout: longer than any copy could take,
     * and short enough that adding it to the current time can't overflow
     * the clock */
    constexpr std::chrono::milliseconds kMaxStageTimeout = std::chrono::hours(24);

    /* The value of environment variable name, or an empty string when it
     * isn't set */
    std::string_view lookupValue(const EnvLookup &lookup, const char *name)
    {
        const char *value = lookup(name);
        return value ? std::string_view(value) : std::string_view();
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

        const Platform &platform() const noexcept
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
     * shut down. Clang still warns, since NoDestructor's empty destructor
     * isn't trivial. */
    HIPOBJ_WARN_NO_EXIT_DTOR_OFF
    static NoDestructor<std::shared_ptr<const Platform>> platform(makeSystemPlatform());
    HIPOBJ_WARN_NO_EXIT_DTOR_ON
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

    const std::string_view requireGpuDirect = lookupValue(lookup, "HIPOBJ_REQUIRE_GPU_DIRECT");
    options.requireGpuDirect = !requireGpuDirect.empty() && !startsWithOneOf(requireGpuDirect, "0nN");

    options.dmabuf.enabled = !startsWithOneOf(lookupValue(lookup, "HIPOBJ_DMABUF"), "0nN");

    const std::string_view debug = lookupValue(lookup, "HIPOBJ_RDMA_DEBUG");
    options.dmabuf.debug         = !debug.empty() && !debug.starts_with('0');
    return options;
}

/* The members are initialized in the order they're declared, so the
 * connection and the buffers use the members, not the arguments moved
 * into them */
Instance::Instance(std::shared_ptr<const Platform> instancePlatform, const Options &instanceOptions,
                   RdmaDevice instanceDevice) noexcept
    : platform(std::move(instancePlatform)), options(instanceOptions), device(std::move(instanceDevice)),
      conn(device), buffers(platform->verbs(), platform->hip(), options.dmabuf, options.requireGpuDirect)
{
}

std::shared_ptr<Instance>
currentInstance()
{
    return current();
}

void
shutdownInstance() noexcept
{
    current().reset();
}

hipObjError_t
initWith(const hipObjConfig_t &config, std::shared_ptr<const Platform> platform, const Options &options)
{
    if (current()) {
        return {hipObjAlreadyInitialized, 0};
    }
    const HipRuntime &runtime     = platform->hip();
    const bool        haveNicHint = config.nicHint && config.nicHint[0] != '\0';
    int               gpuDevice   = config.gpuDevice;
    if (gpuDevice < 0) {
        hipError_t err = runtime.getDevice(&gpuDevice);
        if (err != hipSuccess) {
            // No GPU to infer a device from. That is only fatal when we also have
            // no NIC hint -- with a hint the topology lookup below is skipped
            // entirely, so an absent GPU is not an error.
            if (err != hipErrorNoDevice || !haveNicHint) {
                return {hipObjRdmaError, static_cast<int>(err)};
            }
            gpuDevice = -1;
        }
    }
    else {
        // An explicit device has to exist, even when a NIC hint means its
        // topology isn't needed
        int        deviceCount = 0;
        hipError_t err         = runtime.getDeviceCount(&deviceCount);
        if (err == hipErrorNoDevice) {
            deviceCount = 0;
        }
        else if (err != hipSuccess) {
            return {hipObjRdmaError, static_cast<int>(err)};
        }
        if (gpuDevice >= deviceCount) {
            return {hipObjInvalidValue, 0};
        }
    }
    std::optional<RdmaNic> nic;
    if (gpuDevice >= 0) {
        nic = GetClosestNicToGpu(runtime, platform->topology(), gpuDevice, config.nicHint);
    }
    if (!nic) {
        // GPU topology lookup failed (no GPU or no matching NIC). When a NIC name
        // hint is provided, try opening it directly without GPU topology so the
        // library works in GPU-less environments (e.g. CI with emulated RDMA).
        if (haveNicHint) {
            nic = RdmaNic{0, config.nicHint};
        }
        else {
            return {hipObjNicNotFound, 0};
        }
    }

    /* The device and then the instance are built in locals, which release
     * what they hold if anything fails or throws. The instance is only
     * published once nothing else can. */
    std::optional<RdmaDevice> device =
        openRdmaDeviceByName(platform->verbs(), platform->topology(), nic->name.c_str());
    if (!device) {
        return {hipObjRdmaError, 0};
    }
    auto instance = std::make_shared<Instance>(std::move(platform), options, std::move(*device));
    if (createRcQp(instance->conn, 256, 128, 128) != 0 || transitionQpToInit(instance->conn) != 0) {
        return {hipObjRdmaError, 0};
    }
    instance->gpuDevice = gpuDevice;
    instance->endpoint  = config.endpoint ? config.endpoint : "";
    instance->region    = config.region ? config.region : "";
    instance->nicHint   = config.nicHint ? config.nicHint : "";
    instance->nicIndex  = nic->index;
    instance->flags     = config.flags;

    /* Can't throw */
    current() = std::move(instance);
    return {hipObjSuccess, 0};
}

} // namespace hipObj
