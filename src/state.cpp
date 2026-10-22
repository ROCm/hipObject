/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "state.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <netinet/in.h>

#include "connection-pool.h"
#include "gid.h"
#include "hip-seam.h"
#include "hipobj-private.h"
#include "hipobj-parse.h"
#include "ibv-gid.h"
#include "ibv-wrapper.h"
#include "log.h"
#include "nic-seam.h"
#include "no-destructor.h"
#include "rc-listener.h"
#include "rdma-topology.h"
#include "rdmacm-wrapper.h"
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
            : verbs_(std::move(verbs)), topology_(*verbs_), platform_(hip_, *verbs_, topology_, rdmaCm())
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
#ifdef HIPOBJ_RDMA_WIRE_V3
        SystemRdmaCm rdmaCm_{};

        [[nodiscard]] const RdmaCm *rdmaCm() const noexcept
        {
            return &rdmaCm_;
        }
#else
        /* A v1 build has no listener, and doesn't link librdmacm */
        [[nodiscard]] static const RdmaCm *rdmaCm() noexcept
        {
            return nullptr;
        }
#endif
        Platform platform_;
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

    // Invoked from the RC-CM listener's background thread (rc-listener.h).
    // hipObject is a passive target for the peer's RDMA READ/WRITE, so
    // there is nothing to do beyond what the listener itself already
    // tracks (rcListenerConnectionCount()); kept as named hooks rather
    // than null callbacks so a future need (metrics, logging) has an
    // obvious place to land.
    void onRcListenerConnected(void * /*userCtx*/, void * /*qp*/)
    {
    }

    void onRcListenerDisconnected(void * /*userCtx*/, void * /*qp*/)
    {
    }

    /* hipObject's RC-CM listener (v3) on bindIp and port, through the
     * platform's librdmacm and libibverbs, or null when it can't start
     *
     * TODO: return the running listener when a new instance asks for the
     * same address and port. hipObjShutdown() doesn't wait for transfers
     * in flight, and they keep the old instance, and its listener, alive
     * until they finish, so a hipObjInit() before then binds a port the
     * old listener still holds, and fails. Upstream expects that init to
     * succeed (ShutdownLeavesATransferToFinish in test-thread-safety.cpp),
     * but no test covers it for v3: that test passes only because
     * FakeRdmaCm doesn't refuse a second bind to a port that's in use, and
     * the hardware tests build v1. Sharing needs the instances to own the
     * listener together (a std::shared_ptr, matched on the platform, the
     * address and the port), found through a std::weak_ptr in Current,
     * under initMutex, since nothing else may be global. The fake should
     * then refuse a bind to a port that's in use, so the test covers it. */
    RcListenerPtr startRcListener(const Platform &platform, const std::string &bindIp, uint16_t port)
    {
        const RdmaCm *rdmaCm = platform.rdmaCm();
        if (rdmaCm == nullptr) {
            return nullptr;
        }
        return RcListenerPtr(RdmaCmRcListener::Start(*rdmaCm, platform.verbs(), bindIp.c_str(), port,
                                                     &onRcListenerConnected, &onRcListenerDisconnected,
                                                     nullptr));
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

    options.rcListenIp = std::string(lookupValue(lookup, "HIPOBJ_RC_LISTEN_IP"));
    /* A port that isn't a whole number from 1 to 65535 is ignored */
    if (const auto port = parseNumber<uint16_t>(lookupValue(lookup, "HIPOBJ_RC_LISTEN_PORT"))) {
        if (*port > 0) {
            options.rcListenPort = *port;
        }
    }
    return options;
}

/* The members are initialized in the order they're declared, so the
 * pool and the buffers use the members, not the arguments moved into
 * them */
Instance::Instance(std::shared_ptr<const Platform> instancePlatform, Options instanceOptions,
                   ResolvedConfig instanceConfig, std::shared_ptr<const RdmaDevice> instanceDevice,
                   std::optional<RcConnection> instanceTokenConn, RcListenerPtr instanceListener,
                   std::string instanceListenIp)
    : platform(std::move(instancePlatform)), options(std::move(instanceOptions)),
      config(std::move(instanceConfig)), listenIp(std::move(instanceListenIp)),
      device(std::move(instanceDevice)), listener(std::move(instanceListener)),
      tokenConn(std::move(instanceTokenConn)), pool(device, useV1Wire() ? options.maxTransfers : 0),
      buffers(platform->verbs(), platform->hip(), options.dmabuf, options.requireGpuDirect),
      abandoned(platform->hip())
{
}

std::string
localNicIp(const ibv_gid &localGid)
{
    /* Only an IPv4-mapped GID (::ffff:a.b.c.d, RoCEv2) carries an address */
    const Gid gid = toGid(localGid);
    if (!isIpv4MappedGid(gid)) {
        return {};
    }
    /* The bytes are uint8_t, which std::format prints as numbers */
    const std::span<const uint8_t, 4> ipv4 = ipv4OfGid(gid);
    return std::format("{}.{}.{}.{}", ipv4[0], ipv4[1], ipv4[2], ipv4[3]);
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
    // The RC-CM listener and HIPOBJ_RC_LISTEN_IP are v3-only. v1 has no
    // listener: its peer dials back with the QP number, GID and LID in the
    // token, so v1 always takes the NIC/GPU-topology path below.
    const bool haveListenIp = !useV1Wire() && !options.rcListenIp.empty();
    // The listener binds to the address peers dial, and that bind picks the
    // device: librdmacm opens its own context for it, every accepted
    // connection arrives on that context, and the listener allocates its PD
    // there (see RdmaCmRcListener::Start()). A context hipObject opens itself
    // is never that one, so buffers register on the listener's PD
    // (registrationPd() in hipobj.cpp). With HIPOBJ_RC_LISTEN_IP set, start
    // the listener on it first, and open the device its bind resolved to.
    // Without the listener the server can't dial in, so no v3 transfer could
    // work, and init fails: returning releases what was built.
    RcListenerPtr          listener;
    std::string            listenIp;
    std::optional<RdmaNic> nic;
    if (haveListenIp) {
        listenIp = options.rcListenIp;
        listener = startRcListener(*platform, listenIp, options.rcListenPort);
        if (!listener) {
            logError("hipObjInit: failed to start the RC-CM listener for HIPOBJ_RC_LISTEN_IP={}",
                     listenIp.c_str());
            return makeError(hipObjRdmaError);
        }
        nic = RdmaNic{.index = 0, .name = listener->deviceName()};
    }
    else {
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
    auto device = std::make_shared<const RdmaDevice>(std::move(*opened));
    /* Only v1 advertises a QP of its own */
    std::optional<RcConnection> tokenConn =
        useV1Wire() ? createRcConnection(*device, ConnectionPool::kCqSize, ConnectionPool::kMaxSendWr,
                                         ConnectionPool::kMaxRecvWr)
                    : std::optional<RcConnection>();
    if (useV1Wire() && !tokenConn) {
        return makeError(hipObjRdmaError);
    }

    if (!useV1Wire() && !listener) {
        // hipobj-rc-v3: the RC-CM listener creates one QP per accepted
        // connection (see rc-listener.h), on its own PD, so it can
        // service RDMA READ/WRITE against MRs registered via
        // hipObjBufRegister(). It binds to this NIC's address, and has to
        // end up on this NIC.
        listenIp = localNicIp(device->localGid());
        if (listenIp.empty()) {
            logError("hipObjInit: {} has no IPv4-mapped GID to bind the RC-CM "
                     "listener to; set HIPOBJ_RC_LISTEN_IP",
                     nic->name.c_str());
            return makeError(hipObjRdmaError);
        }
        listener = startRcListener(*platform, listenIp, options.rcListenPort);
        if (!listener) {
            logError("hipObjInit: failed to start the RC-CM listener on {} port {}", listenIp.c_str(),
                     static_cast<unsigned>(options.rcListenPort));
            return makeError(hipObjRdmaError);
        }
        if (listener->deviceName() != nic->name) {
            logError("hipObjInit: {} is on {}, not the chosen NIC {}", listenIp.c_str(),
                     listener->deviceName().c_str(), nic->name.c_str());
            return makeError(hipObjRdmaError);
        }
    }

    ResolvedConfig resolved{
        .gpuDevice = gpuDevice,
        .nicHint   = std::string(nicHint),
        .nicIndex  = nic->index,
    };
    auto instance =
        std::make_shared<Instance>(std::move(platform), options, std::move(resolved), std::move(device),
                                   std::move(tokenConn), std::move(listener), std::move(listenIp));
    /* The pool stops at the first connection it can't create */
    if (useV1Wire() && instance->pool.size() != options.maxTransfers) {
        return makeError(hipObjRdmaError);
    }

    /* If taking the lock throws, the instance is released, and nothing
     * has changed. Moving the instance in can't throw. */
    const ScopedLock<SharedMutex> lock(state.mutex);
    state.instance = std::move(instance);
    return makeError(hipObjSuccess);
}

} // namespace hipObj
