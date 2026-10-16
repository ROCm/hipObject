/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * A fake RDMA device and HIP runtime for tests of the public API. The
 * fixture initializes the library with a FakePlatform, whose fakes stand
 * in for libibverbs, the HIP runtime, and the NIC topology, so
 * initialization and transfers run without GPU or RDMA hardware. The
 * fakes record their calls and can be told to fail (see FakeDevice).
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "state.h"
#include "token.h"

namespace hipObjTest {

/* The calls the fakes have seen. The verbs objects are counted as they're
 * created (successfully) and destroyed, so a test can check that none
 * leaked. The counters are atomic, so the fakes can be called from
 * several threads at once. */
struct CallLog {
    std::atomic<int> sendRequest{0};
    std::atomic<int> recvReply{0};
    std::atomic<int> registerMr{0};
    std::atomic<int> mrAccess{0}; /* the access flags of the last reg_mr */
    std::atomic<int> deregMr{0};
    std::atomic<int> openDevice{0};
    std::atomic<int> closeDevice{0};
    std::atomic<int> freeDeviceList{0};
    std::atomic<int> queryDevice{0}; /* every call, including failed ones */
    std::atomic<int> allocPd{0};
    std::atomic<int> deallocPd{0};
    std::atomic<int> createCq{0};
    std::atomic<int> destroyCq{0};
    std::atomic<int> createQp{0};
    std::atomic<int> destroyQp{0};
    std::atomic<int> postSend{0};
    std::atomic<int> pollCq{0};

    /* HIP and HSA */
    std::atomic<int> memcpy{0}; /* memcpyAsync() */
    std::atomic<int> eventCreate{0};
    std::atomic<int> eventRecord{0};
    std::atomic<int> eventQuery{0};
    std::atomic<int> eventDestroy{0};
    std::atomic<int> hostMalloc{0};
    std::atomic<int> hostFree{0};
    std::atomic<int> deviceSynchronize{0};
    std::atomic<int> exportDmabuf{0};
    std::atomic<int> closeDmabuf{0};
};

/* Which fakes fail */
struct Faults {
    bool getDeviceList = false;
    bool openDevice    = false;
    bool allocPd       = false;
    bool queryPort     = false;
    bool queryGid      = false;
    bool queryDevice   = false;
    bool createCq      = false;
    bool createQp      = false;
    /* create_qp fails once it's created this many QPs, if it's set */
    int qpLimit = -1;
    /* modify_qp fails for a transition to this state */
    int modifyQpToState = -1;
    /* reg_mr fails for this address, so hipObjBufRegister() falls back to a
     * host staging buffer */
    void *registerAddr = nullptr;
    /* reg_mr fails for every address, the staging buffer's too */
    bool registerAll  = false;
    bool hostMalloc   = false;
    bool exportDmabuf = false;
};

/* One modify_qp() call */
struct ModifyQpCall {
    ibv_qp_attr attr{};
    int         mask = 0;
};

/* What the fakes return, and what they record. The fixture resets it before
 * each test (see resetFake()). Trivially destructible, so it can live in a
 * static.
 *
 * The fields the fakes write are atomic, and the fakes only read the
 * others, which a test sets before it starts any threads, so the fakes can
 * be called from several threads at once. */
struct FakeDevice {
    CallLog log{};
    Faults  fail{};

    /* HIP: the GPUs, and what the runtime reports for every pointer (its
     * memory type and the allocation it's in). There's no current device,
     * and no GPU's bus ID is known, so hipObjInit() opens the hinted NIC.
     * Every pointer is host memory, so registration doesn't look for the
     * allocation. */
    hipError_t          getDeviceErr    = hipErrorNoDevice;
    int                 device          = 0;
    int                 deviceCount     = 1;
    hipError_t          deviceCountErr  = hipSuccess;
    std::string_view    gpuBusId        = {};
    hipError_t          deviceSyncErr   = hipSuccess;
    hipMemoryType       memoryType      = hipMemoryTypeHost;
    uintptr_t           allocBase       = 0;
    size_t              allocSize       = 0;
    hipError_t          addressRangeErr = hipSuccess;
    std::atomic<int>    addressRangeCalls{0};
    std::atomic<void *> lastHostMalloc{nullptr};
    std::atomic<void *> lastHostFree{nullptr};

    /* HIP staging copies: memcpyAsync() and the event that tracks it.
     * eventQuery() reports hipErrorNotReady for the first
     * eventNotReadyCount queries of each event, and then
     * eventQueryResult; with eventNeverCompletes, it reports
     * hipErrorNotReady forever. lastMemcpyStream and lastEventRecordStream
     * hold the streams the last copy and the last eventRecord() used. */
    hipError_t                 memcpyErr = hipSuccess;
    std::atomic<hipMemcpyKind> lastMemcpyKind{hipMemcpyDefault};
    std::atomic<hipStream_t>   lastMemcpyStream{nullptr};
    std::atomic<hipStream_t>   lastEventRecordStream{nullptr};
    hipError_t                 eventCreateErr      = hipSuccess;
    hipError_t                 eventRecordErr      = hipSuccess;
    hipError_t                 eventQueryResult    = hipSuccess;
    int                        eventNotReadyCount  = 0;
    bool                       eventNeverCompletes = false;
    std::atomic<int>           eventQueriesAtCreate{0}; /* log.eventQuery when the last event was created */

    /* HSA: the fd closeDmabuf() was last given */
    std::atomic<int> closedDmabufFd{-1};

    /* ibverbs: the vendor query_device() reports, what poll_cq() returns,
     * and the QP transitions. modifyQpCount counts the modify_qp() calls,
     * and modifyQp holds the first ones; read it only once no thread is
     * calling modify_qp(). Each QP returned by create_qp() gets the next
     * nextQpNum value. */
    uint32_t                    vendorId   = 0;
    int                         pollResult = 1;
    enum ibv_wc_status          pollStatus = IBV_WC_SUCCESS;
    enum ibv_wc_opcode          pollOpcode = IBV_WC_SEND;
    std::array<ModifyQpCall, 8> modifyQp{};
    std::atomic<size_t>         modifyQpCount{0};
    std::atomic<uint32_t>       nextQpNum{0x55};

    /* The S3 server's reply to a transfer (see setReply()) */
    std::array<char, 128> reply    = {{'2', '0', '0', '\0'}};
    size_t                replyLen = 4;
};

FakeDevice &fake();

/* Puts fake() back the way a new FakeDevice is */
void resetFake();

/* Sets the reply the fake S3 server sends, with its NUL, as the default "200"
 * is sent */
void setReply(std::string_view reply);

/* A reply that carries a peer token, which makes the transfer connect the
 * QP to the peer */
hipObj::RdmaToken peerToken(hipObj::TransportType transport = hipObj::TransportType::RC);
void              setReplyWithPeerToken(const hipObj::RdmaToken &token);

/* The modify_qp() calls so far, up to the number modifyQp holds */
std::vector<ModifyQpCall> modifyQpCalls();

/* The event FakeHip creates, and the dma-buf it exports */
inline hipEvent_t
fakeEvent()
{
    return hipObj::addressToPointer<hipEvent_t>(0x99);
}

constexpr int      kFakeDmabufFd     = 1234;
constexpr uint64_t kFakeDmabufOffset = 0x200;

/* Frees memory from FakeHip::hostMalloc(), as hostFree() does, without
 * recording a call. A test can free what the library leaks with it. */
void freeFakeHostMemory(void *ptr);

/* A HIP runtime that answers from a FakeDevice and records its calls there.
 * hostMalloc() and hostFree() allocate and free real memory. A test can
 * derive from it to change one method. */
class FakeHip : public hipObj::HipRuntime {
public:
    explicit FakeHip(FakeDevice &device) noexcept : device_(&device)
    {
    }
    ~FakeHip() override = default;

    FakeHip(const FakeHip &)            = delete;
    FakeHip &operator=(const FakeHip &) = delete;
    FakeHip(FakeHip &&)                 = delete;
    FakeHip &operator=(FakeHip &&)      = delete;

    hipError_t               getDevice(int *device) const override;
    hipError_t               getDeviceCount(int *count) const override;
    hipError_t               deviceGetPCIBusId(char *pciBusId, int len, int device) const override;
    hipError_t               hostMalloc(void **ptr, size_t size, unsigned int flags) const override;
    hipError_t               hostFree(void *ptr) const override;
    [[nodiscard]] hipError_t deviceSynchronize() const override;
    hipError_t               memcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind,
                                         hipStream_t stream) const override;
    hipError_t               eventCreate(hipEvent_t *event) const override;
    hipError_t               eventRecord(hipEvent_t event, hipStream_t stream) const override;
    hipError_t               eventQuery(hipEvent_t event) const override;
    hipError_t               eventDestroy(hipEvent_t event) const override;
    hipError_t pointerGetAttributes(hipPointerAttribute_t *attributes, const void *ptr) const override;
    hipError_t memGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t ptr) const override;
    int        exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const override;
    [[nodiscard]] int closeDmabuf(int fd) const override;

private:
    /* A pointer, so the const methods can change what it points to */
    FakeDevice *device_;
};

/* A libibverbs that answers from a FakeDevice and records its calls there.
 * It has one device, and no ibv_reg_dmabuf_mr(). reg_mr(), create_cq(),
 * and create_qp() allocate real objects, each distinct, which dereg_mr(),
 * destroy_cq(), and destroy_qp() free; the other objects are made-up
 * addresses. A CQ's cqe is the size it was created with. modify_qp()
 * keeps each QP's state in its state member, and rejects a transition
 * that real verbs would, such as RTS to RTR. A test can derive from it to
 * change some of its methods. */
class FakeVerbs : public hipObj::Verbs {
public:
    explicit FakeVerbs(FakeDevice &device) noexcept : device_(&device)
    {
    }
    ~FakeVerbs() override = default;

    FakeVerbs(const FakeVerbs &)            = delete;
    FakeVerbs &operator=(const FakeVerbs &) = delete;
    FakeVerbs(FakeVerbs &&)                 = delete;
    FakeVerbs &operator=(FakeVerbs &&)      = delete;

    struct ibv_device **get_device_list(int *num_devices) const override;
    void                free_device_list(struct ibv_device **list) const override;
    struct ibv_context *open_device(struct ibv_device *device) const override;
    int                 close_device(struct ibv_context *context) const override;
    const char         *get_device_name(struct ibv_device *device) const override;
    int query_device(struct ibv_context *context, struct ibv_device_attr *device_attr) const override;
    int query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr) const override;
    int query_gid(struct ibv_context *context, uint8_t port_num, int index,
                  union ibv_gid *gid) const override;
    struct ibv_pd     *alloc_pd(struct ibv_context *context) const override;
    int                dealloc_pd(struct ibv_pd *pd) const override;
    struct ibv_mr     *reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access) const override;
    [[nodiscard]] bool supportsDmabuf() const override;
    struct ibv_mr     *reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd,
                                     int access) const override;
    int                dereg_mr(struct ibv_mr *mr) const override;
    struct ibv_cq     *create_cq(struct ibv_context *context, int cqe, void *cq_context,
                                 struct ibv_comp_channel *channel, int comp_vector) const override;
    int                destroy_cq(struct ibv_cq *cq) const override;
    struct ibv_qp     *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) const override;
    int                modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask) const override;
    int                destroy_qp(struct ibv_qp *qp) const override;
    int                poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc) const override;
    int post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr) const override;

private:
    /* A pointer, so the const methods can change what it points to */
    FakeDevice *device_;
};

/* hipObjOps_t callbacks for the fake S3 server */
int         fakeSendRequest(void *ctx, const char *token, size_t tokenLen);
int         fakeRecvReply(void *ctx, char *reply, size_t *replyLen);
hipObjOps_t makeOps();

/* A fake device address. reg_mr is faked, so nothing reads or writes it. */
constexpr uintptr_t kFakeDevBufAddr = 0x10000;

inline void *
fakeDevBuf()
{
    return hipObj::addressToPointer<void *>(kFakeDevBufAddr);
}

/* A fake address one byte into fakeDevBuf(). It's computed as an integer,
 * since there's no object at the fake address to do pointer arithmetic
 * in. */
inline void *
fakeDevBufPlusOne()
{
    return hipObj::addressToPointer<void *>(kFakeDevBufAddr + 1);
}

/* Another fake device address, for a buffer that's registered through a
 * staging buffer. Tests make reg_mr fail for it, by setting
 * fake().fail.registerAddr to it. */
constexpr uintptr_t kFakeStagedBufAddr = 0x30000;

inline void *
fakeStagedBuf()
{
    return hipObj::addressToPointer<void *>(kFakeStagedBufAddr);
}

constexpr size_t kFakeBufSize = 64;

/* A NIC topology with no NICs, and no RoCE version for any GID */
class EmptyNicTopology : public hipObj::NicTopology {
public:
    [[nodiscard]] std::vector<hipObj::NicInfo> enumerate(std::string_view hcaList) const override;
    [[nodiscard]] hipObj::RoceVersion          roceVersion(std::string_view devName, uint8_t portNum,
                                                           int gidIndex) const override;
};

/* Fakes of the outside world over a FakeDevice, and a platform over them
 * for an instance to call. Hip and Verbs can be fakes a test derived from
 * FakeHip or FakeVerbs to change what they do. Build it with
 * std::make_shared: platform() shares ownership of it, so the fakes live
 * as long as an instance that calls them. */
template <typename Hip = FakeHip, typename Verbs = FakeVerbs>
class FakePlatform final : public std::enable_shared_from_this<FakePlatform<Hip, Verbs>> {
public:
    /* device has to outlive it */
    explicit FakePlatform(FakeDevice &device) noexcept
        : hip_(device), verbs_(device), platform_(hip_, verbs_, topology_)
    {
    }

    ~FakePlatform() = default;

    /* The platform refers to the members */
    FakePlatform(const FakePlatform &)            = delete;
    FakePlatform &operator=(const FakePlatform &) = delete;
    FakePlatform(FakePlatform &&)                 = delete;
    FakePlatform &operator=(FakePlatform &&)      = delete;

    /* The platform over the fakes, which keeps this alive */
    [[nodiscard]] std::shared_ptr<const hipObj::Platform> platform() const
    {
        /* Points to the platform, and shares ownership of this. The base
         * class depends on the template's arguments, so its members are
         * named through this. */
        return {this->shared_from_this(), &platform_};
    }

    [[nodiscard]] const Hip &hip() const noexcept
    {
        return hip_;
    }

    [[nodiscard]] const Verbs &verbs() const noexcept
    {
        return verbs_;
    }

    [[nodiscard]] const EmptyNicTopology &topology() const noexcept
    {
        return topology_;
    }

private:
    Hip              hip_;
    Verbs            verbs_;
    EmptyNicTopology topology_{};
    hipObj::Platform platform_;
};

/* initWith() with config, platform, and options. hipObjInit() always
 * uses the system platform, so this is how a test initializes the library
 * with fakes. */
hipObjError_t initWithPlatform(const hipObjConfig_t &config, std::shared_ptr<const hipObj::Platform> platform,
                               const hipObj::Options &options = {});

/* Gives each test a FakePlatform over fake(), and resets fake(). After each
 * test, it shuts the library down, and checks that the instance is gone
 * and that nothing but the fixture still holds the fakes. */
class FakeDeviceTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    static hipObjConfig_t makeConfig();

    /* initWithPlatform() with the fakes */
    [[nodiscard]] hipObjError_t initFakes(const hipObjConfig_t  &config,
                                          const hipObj::Options &options = {}) const;

    /* Initializes the library with the fakes */
    void init() const;

    /* Initializes and registers kFakeBufSize bytes at fakeDevBuf() */
    void initAndRegister() const;

    [[nodiscard]] const hipObj::Verbs &verbs() const noexcept
    {
        return fakes_->verbs();
    }

    [[nodiscard]] const hipObj::NicTopology &topology() const noexcept
    {
        return fakes_->topology();
    }

    std::shared_ptr<FakePlatform<>> fakes_ = std::make_shared<FakePlatform<>>(fake());
};

} // namespace hipObjTest
