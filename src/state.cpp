/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "hip-seam.h"
#include "hipobj-parse.h"
#include "hipobj-warnings.h"
#include "ibv-wrapper.h"
#include "log.h"
#include "nic-seam.h"
#include "no-destructor.h"
#include "rc-listener.h"
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

    /* The platform systemPlatform() returns, and the SysfsTopology it
     * uses, when no test installed a topology. sysfs_ is declared first,
     * so it's built before platform_ refers to it. */
    class SystemPlatform final {
    public:
        SystemPlatform() : platform_(hip(), ibv(), currentNicTopology(sysfs_))
        {
        }

        const Platform &platform() const noexcept
        {
            return platform_;
        }

    private:
        std::optional<SysfsTopology> sysfs_{};
        Platform                     platform_;
    };

} // namespace

std::shared_ptr<const Platform>
systemPlatform()
{
    /* Points to the platform, and shares ownership of the SystemPlatform
     * that holds it, so the SysfsTopology lives as long as the platform
     * does */
    const auto system = std::make_shared<SystemPlatform>();
    return {system, &system->platform()};
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

Instance::Instance(std::shared_ptr<const Platform> instancePlatform, const Options &instanceOptions) noexcept
    : platform(std::move(instancePlatform)), options(instanceOptions),
      buffers(instanceOptions.dmabuf, instanceOptions.requireGpuDirect)
{
}

Instance::~Instance()
{
    RcListener::Stop(listener);
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

std::string
localNicIp(const ibv_gid &localGid)
{
    const char *envIp = std::getenv("HIPOBJ_RC_LISTEN_IP");
    if (envIp && envIp[0] != '\0') {
        LOG_DEBUG("using HIPOBJ_RC_LISTEN_IP override: %s", envIp);
        return std::string(envIp);
    }
    const uint8_t *gid = localGid.raw;
    if (gid[10] != 0xff || gid[11] != 0xff) {
        return std::string();
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", gid[12], gid[13], gid[14], gid[15]);
    return std::string(buf);
}

// Invoked from the RC-CM listener's background thread (rc-listener.h).
// hipObject is a passive target for the peer's RDMA READ/WRITE, so
// there is nothing to do beyond what the listener itself already
// tracks (rcListenerConnectionCount()); kept as named hooks rather
// than null callbacks so a future need (metrics, logging) has an
// obvious place to land.
static void
onRcListenerConnected(void * /*userCtx*/, void * /*qp*/)
{
}

static void
onRcListenerDisconnected(void * /*userCtx*/, void * /*qp*/)
{
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
    /* The instance is built in a local, which releases what it holds if
     * anything fails or throws. It's only published once nothing else
     * can. */
    auto instance = std::make_shared<Instance>(std::move(platform), options);

    bool listenerSelfManaged = false;
    // HIPOBJ_RC_LISTEN_IP (see localNicIp() in state.h) is the address
    // the RC-CM listener will advertise and bind to. A struct ibv_context
    // this process opens on its own via ibv_open_device() (as
    // openRdmaDevice()/openRdmaDeviceByName() do) is never the same
    // context RDMA CM hands an accepted connection as id->verbs, even for
    // the identical physical device -- libibverbs gives every open() its
    // own context, and a PD from one can't back a QP on another. So when
    // the override is set, start the RC-CM listener FIRST in
    // self-managed mode (see RcListener::Start()): it binds to this IP,
    // lets RDMA CM pick the device, and allocates its own PD from that
    // exact context. instance->conn.ctx/pd are NOT pointed at the
    // listener's device/PD here: RcConnection now owns them through
    // IbvContextPtr/IbvPdPtr, and aliasing an owning smart pointer at a PD
    // the listener itself owns would double-free once RcListener::Stop()
    // releases it (see ~Instance()). registrationPd()/registrationCtx() in
    // hipobj.cpp consult listenerSelfManaged to read the real device/PD
    // from instance->listener instead, so buffer registration still
    // matches what accepted connections get.
    // HIPOBJ_RC_LISTEN_IP and the RC-CM listener are v3-only concepts (v1
    // has no listener, and its peer dials back via QP/GID/LID exchanged
    // directly in the token) -- v1 mode always takes the plain
    // NIC/GPU-topology device-open path below instead.
    const char *rcListenIpOverride = useV1Wire() ? nullptr : std::getenv("HIPOBJ_RC_LISTEN_IP");
    if (rcListenIpOverride && rcListenIpOverride[0] != '\0') {
        instance->listener = RcListener::Start(nullptr, nullptr, rcListenIpOverride, 0,
                                               &onRcListenerConnected, &onRcListenerDisconnected, nullptr);
        listenerSelfManaged =
            instance->listener && instance->listener->deviceCtx() && instance->listener->pd();
        if (!listenerSelfManaged) {
            LOG_WARN("hipObjInit: failed to self-manage RC-CM listener for "
                     "HIPOBJ_RC_LISTEN_IP=%s; falling back to NIC/GPU topology",
                     rcListenIpOverride);
        }
    }
    std::optional<RdmaNic> nic;
    if (!listenerSelfManaged) {
        if (gpuDevice >= 0) {
            nic = GetClosestNicToGpu(gpuDevice, config.nicHint);
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
        if (openRdmaDeviceByName(nic->name.c_str(), instance->conn) != 0) {
            return {hipObjRdmaError, 0};
        }
        if (useV1Wire()) {
            // v1: the peer brings its own QP to RTR/RTS against ours (see
            // buildRdmaToken()/finishTransferAfterReply()), so unlike v3 this
            // needs an ahead-of-time QP, created and left in INIT state here.
            if (createRcQp(instance->conn, 256, 128, 128) != 0 || transitionQpToInit(instance->conn) != 0) {
                return {hipObjRdmaError, 0};
            }
        }
        else {
            // hipobj-rc-v3: the RC-CM listener creates one QP per
            // accepted connection (see rc-listener.h), against
            // this same PD so it can service RDMA READ/WRITE against MRs
            // registered via hipObjBufRegister(). Non-fatal: a bind failure
            // only disables the server dial-in path, not hipObjBufRegister
            // or the rest of hipObjInit.
            std::string rcListenIp = localNicIp(instance->conn.localGid);
            instance->listener =
                RcListener::Start(instance->conn.pd.get(), instance->conn.ctx.get(),
                                  rcListenIp.empty() ? nullptr : rcListenIp.c_str(), 0,
                                  &onRcListenerConnected, &onRcListenerDisconnected, nullptr);
            if (!instance->listener) {
                LOG_WARN("hipObjInit: failed to start RC-CM listener on port %u; "
                         "continuing without it",
                         rcListenerPortFromEnv());
            }
        }
    }
    instance->gpuDevice           = gpuDevice;
    instance->endpoint            = config.endpoint ? config.endpoint : "";
    instance->region              = config.region ? config.region : "";
    instance->nicHint             = config.nicHint ? config.nicHint : "";
    instance->nicIndex            = nic ? nic->index : -1;
    instance->flags               = config.flags;
    instance->listenerSelfManaged = listenerSelfManaged;

    /* Can't throw */
    current() = std::move(instance);
    return {hipObjSuccess, 0};
}

} // namespace hipObj
