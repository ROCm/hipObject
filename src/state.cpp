/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>

#include "hip-seam.h"
#include "hipobj-private.h"
#include "hipobj-parse.h"
#include "ibv-wrapper.h"
#include "log.h"
#include "nic-seam.h"
#include "no-destructor.h"
#include "rc-listener.h"
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

Instance::~Instance()
{
    /* The buffers first: with the RC-CM listener running, their MRs are on
     * its PD (registrationPd() in hipobj.cpp), which Stop() deallocates, and
     * a PD with MRs left on it can't be */
    buffers.deregisterAll();
    listener.reset();
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

std::string
localNicIp(const ibv_gid &localGid)
{
    /* Only an IPv4-mapped GID (::ffff:a.b.c.d, RoCEv2) carries an address */
    const std::span<const uint8_t, 16> gid(localGid.raw);
    if (gid[10] != 0xff || gid[11] != 0xff) {
        return std::string();
    }
    std::array<char, INET_ADDRSTRLEN> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%u.%u.%u.%u", static_cast<unsigned>(gid[12]),
                                static_cast<unsigned>(gid[13]), static_cast<unsigned>(gid[14]),
                                static_cast<unsigned>(gid[15]));
    if (n < 0 || static_cast<size_t>(n) >= buf.size()) {
        return std::string();
    }
    return std::string(buf.data());
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

/* The function installed with setRcListenerStartForTest(), or null */
static RcListenerStart g_rcListenerStartOverride = nullptr;

void
RcListenerDeleter::operator()(RcListener *listener) const noexcept
{
    listener->stop();
}

RcListenerPtr
startRcListener(const char *bindIp, uint16_t port)
{
    if (g_rcListenerStartOverride) {
        return RcListenerPtr(g_rcListenerStartOverride(bindIp, port));
    }
    return RcListenerPtr(
        RdmaCmRcListener::Start(bindIp, port, &onRcListenerConnected, &onRcListenerDisconnected, nullptr));
}

RcListenerStart
setRcListenerStartForTest(RcListenerStart start)
{
    const RcListenerStart previous = g_rcListenerStartOverride;
    g_rcListenerStartOverride      = start;
    return previous;
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
    /* The instance is built in a local, which releases what it holds if
     * anything fails or throws. It's only published once nothing else
     * can. */
    auto instance = std::make_shared<Instance>(std::move(platform), options);

    // The RC-CM listener and HIPOBJ_RC_LISTEN_IP are v3-only. v1 has no
    // listener: its peer dials back with the QP number, GID and LID in the
    // token, so v1 always takes the NIC/GPU-topology path below.
    const char *rcListenIpOverride = useV1Wire() ? nullptr : std::getenv("HIPOBJ_RC_LISTEN_IP");
    const bool  haveListenIp       = rcListenIpOverride && rcListenIpOverride[0] != '\0';
    // The listener binds to the address peers dial, and that bind picks the
    // device: librdmacm opens its own context for it, every accepted
    // connection arrives on that context, and the listener allocates its PD
    // there (see RdmaCmRcListener::Start()). A context hipObject opens itself is
    // never that one, so buffers register on the listener's PD
    // (registrationPd() in hipobj.cpp). With HIPOBJ_RC_LISTEN_IP set, start
    // the listener on it first: once it's running, hipObject needs no
    // device of its own. Without the listener the server can't dial in, so
    // no v3 transfer could work, and init fails: returning releases what the
    // instance holds.
    if (haveListenIp) {
        instance->listenIp = rcListenIpOverride;
        instance->listener = startRcListener(rcListenIpOverride, 0);
        if (!instance->listener) {
            LOG_ERROR("hipObjInit: failed to start the RC-CM listener for HIPOBJ_RC_LISTEN_IP=%s",
                      rcListenIpOverride);
            return {hipObjRdmaError, 0};
        }
    }
    std::optional<RdmaNic> nic;
    if (!instance->listener) {
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
            // hipobj-rc-v3: the RC-CM listener creates one QP per accepted
            // connection (see rc-listener.h), on its own PD, so it can
            // service RDMA READ/WRITE against MRs registered via
            // hipObjBufRegister(). It binds to this NIC's address, and has to
            // end up on this NIC.
            instance->listenIp = localNicIp(instance->conn.localGid);
            if (instance->listenIp.empty()) {
                LOG_ERROR("hipObjInit: %s has no IPv4-mapped GID to bind the RC-CM "
                          "listener to; set HIPOBJ_RC_LISTEN_IP",
                          nic->name.c_str());
                return {hipObjRdmaError, 0};
            }
            instance->listener = startRcListener(instance->listenIp.c_str(), 0);
            if (!instance->listener) {
                LOG_ERROR("hipObjInit: failed to start the RC-CM listener on %s port %u",
                          instance->listenIp.c_str(), rcListenerPortFromEnv());
                return {hipObjRdmaError, 0};
            }
            if (instance->listener->deviceName() != nic->name) {
                LOG_ERROR("hipObjInit: %s is on %s, not the chosen NIC %s", instance->listenIp.c_str(),
                          instance->listener->deviceName().c_str(), nic->name.c_str());
                return {hipObjRdmaError, 0};
            }
        }
    }
    instance->gpuDevice = gpuDevice;
    instance->endpoint  = config.endpoint ? config.endpoint : "";
    instance->region    = config.region ? config.region : "";
    instance->nicHint   = config.nicHint ? config.nicHint : "";
    instance->nicIndex  = nic ? nic->index : -1;
    instance->flags     = config.flags;

    /* Can't throw */
    current() = std::move(instance);
    return {hipObjSuccess, 0};
}

} // namespace hipObj
