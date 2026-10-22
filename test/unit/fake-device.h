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
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#ifdef HIPOBJ_CPU_RDMA
#include "hip-shim.h"
#else
#include <hip/hip_runtime_api.h>
#endif

#include "hip-seam.h"
#include "hipobj-address.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "rdmacm-core.h"
#include "rdmacm-wrapper.h"
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

    /* librdmacm (FakeRdmaCm), and the async events FakeVerbs reports. The
     * ids include the ones connectRequest() makes, as librdmacm makes an
     * id for each connection. */
    std::atomic<int> createEventChannel{0};
    std::atomic<int> destroyEventChannel{0};
    std::atomic<int> createId{0};
    std::atomic<int> destroyId{0};
    std::atomic<int> bindAddr{0};
    std::atomic<int> listen{0};
    std::atomic<int> ackCmEvent{0};
    std::atomic<int> accept{0};
    std::atomic<int> reject{0};
    std::atomic<int> disconnect{0};
    std::atomic<int> ackAsyncEvent{0};
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

    /* librdmacm (FakeRdmaCm). With bindResolvesNoDevice, a bind succeeds
     * but resolves no device, as a bind to a wildcard address does. */
    bool createEventChannel   = false;
    bool createId             = false;
    bool bindAddr             = false;
    bool bindResolvesNoDevice = false;
    bool listen               = false;
    bool cmCreateQp           = false;
    bool accept               = false;
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
    ibv_wc_status               pollStatus = IBV_WC_SUCCESS;
    ibv_wc_opcode               pollOpcode = IBV_WC_SEND;
    std::array<ModifyQpCall, 8> modifyQp{};
    std::atomic<size_t>         modifyQpCount{0};
    std::atomic<uint32_t>       nextQpNum{0x55};
    /* The GID query_gid() reports for every index */
    std::array<uint8_t, 16> gid{};

    /* librdmacm (see FakeRdmaCm): binding to listenerAddress resolves to
     * the device the fake verbs call listenerDevice. asyncEventType is the
     * async event its context reports. */
    std::string_view listenerAddress = "192.0.2.1";
    const char      *listenerDevice  = "mlx5_0";
    ibv_event_type   asyncEventType  = IBV_EVENT_QP_FATAL;

    /* The S3 server's reply to a transfer (see setReply()) */
    std::array<char, 128> reply    = {{'2', '0', '0', '\0'}};
    size_t                replyLen = 4;
    /* Send successReply() rather than reply, until setReply() is called.
     * FakeDeviceTest sets it for v3, whose success reply has to carry the
     * transfer's length. */
    bool replyWithSuccess = false;
    /* The length the last token named (see noteSentToken()) */
    std::atomic<uint64_t> lastTokenLength{0};
};

FakeDevice &fake();

/* Puts fake() back the way a new FakeDevice is */
void resetFake();

/* The GID FakeDeviceTest gives the fake NIC under v3: IPv4-mapped
 * ::ffff:192.0.2.1, the listenerAddress the RC-CM listener binds to */
inline constexpr auto kFakeNicGid =
    std::to_array<uint8_t>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1});

/* Sets the reply the fake S3 server sends, with its NUL, as the default "200"
 * is sent */
void setReply(std::string_view reply);

/* Records the length the token a fake sendRequest was given names, as the
 * real server learns it from the token. Every fake sendRequest calls it,
 * so that successReply() can report it. */
void noteSentToken(const char *token, size_t tokenLen);

/* The fake S3 server's success reply for this wire: "200" for v1, and for
 * v3 "200:" and the length noteSentToken() last recorded. hipObject only
 * accepts a v3 reply that reports the whole transfer. */
std::string successReply();

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

    [[nodiscard]] hipError_t getDevice(int *device) const override;
    [[nodiscard]] hipError_t getDeviceCount(int *count) const override;
    [[nodiscard]] hipError_t deviceGetPCIBusId(char *pciBusId, int len, int device) const override;
    [[nodiscard]] hipError_t hostMalloc(void **ptr, size_t size, unsigned int flags) const override;
    [[nodiscard]] hipError_t hostFree(void *ptr) const override;
    [[nodiscard]] hipError_t deviceSynchronize() const override;
    [[nodiscard]] hipError_t memcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind,
                                         hipStream_t stream) const override;
    [[nodiscard]] hipError_t eventCreate(hipEvent_t *event) const override;
    [[nodiscard]] hipError_t eventRecord(hipEvent_t event, hipStream_t stream) const override;
    [[nodiscard]] hipError_t eventQuery(hipEvent_t event) const override;
    [[nodiscard]] hipError_t eventDestroy(hipEvent_t event) const override;
    [[nodiscard]] hipError_t pointerGetAttributes(hipPointerAttribute_t *attributes,
                                                  const void            *ptr) const override;
    [[nodiscard]] hipError_t memGetAddressRange(hipDeviceptr_t *base, size_t *size,
                                                hipDeviceptr_t ptr) const override;
    [[nodiscard]] int exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const override;
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

    [[nodiscard]] ibv_device **get_device_list(int *num_devices) const override;
    void                       free_device_list(ibv_device **list) const override;
    [[nodiscard]] ibv_context *open_device(ibv_device *device) const override;
    [[nodiscard]] int          close_device(ibv_context *context) const override;
    [[nodiscard]] const char  *get_device_name(ibv_device *device) const override;
    [[nodiscard]] int     query_device(ibv_context *context, ibv_device_attr *device_attr) const override;
    [[nodiscard]] int     query_port(ibv_context *context, uint8_t port_num,
                                     ibv_port_attr *port_attr) const override;
    [[nodiscard]] int     query_gid(ibv_context *context, uint8_t port_num, int index,
                                    ibv_gid *gid) const override;
    [[nodiscard]] ibv_pd *alloc_pd(ibv_context *context) const override;
    [[nodiscard]] int     dealloc_pd(ibv_pd *pd) const override;
    [[nodiscard]] ibv_mr *reg_mr(ibv_pd *pd, void *addr, size_t length, int access) const override;
    [[nodiscard]] bool    supportsDmabuf() const override;
    [[nodiscard]] ibv_mr *reg_dmabuf_mr(ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd,
                                        int access) const override;
    [[nodiscard]] int     dereg_mr(ibv_mr *mr) const override;
    [[nodiscard]] ibv_cq *create_cq(ibv_context *context, int cqe, void *cq_context,
                                    ibv_comp_channel *channel, int comp_vector) const override;
    [[nodiscard]] int     destroy_cq(ibv_cq *cq) const override;
    [[nodiscard]] ibv_qp *create_qp(ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const override;
    [[nodiscard]] int     modify_qp(ibv_qp *qp, ibv_qp_attr *attr, int attr_mask) const override;
    [[nodiscard]] int     destroy_qp(ibv_qp *qp) const override;
    [[nodiscard]] int     poll_cq(ibv_cq *cq, int num_entries, ibv_wc *wc) const override;
    [[nodiscard]] int     post_send(ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) const override;
    /* For FakeRdmaCm's context, whose async fd counts the events: reports
     * the device's asyncEventType */
    [[nodiscard]] int         get_async_event(ibv_context *context, ibv_async_event *event) const override;
    void                      ack_async_event(ibv_async_event *event) const override;
    [[nodiscard]] const char *event_type_str(ibv_event_type event) const override;

private:
    /* A pointer, so the const methods can change what it points to */
    FakeDevice *device_;
};

/* The device FakeRdmaCm's context is on, which FakeVerbs names the
 * FakeDevice's listenerDevice */
inline ibv_device *
fakeListenerDevice()
{
    return hipObj::addressToPointer<ibv_device *>(0x5);
}

/* A librdmacm that answers from a FakeDevice and records its calls there,
 * for the RC-CM listener (v3). Binding to the device's listenerAddress
 * resolves to a context of its own, as librdmacm's is, on
 * fakeListenerDevice(). Its channels' fds, and that context's async fd,
 * are eventfds, so the listener's epoll loop runs as it does on hardware.
 * A test queues the events a listener gets with connectRequest() and
 * queueEvent(). QPs are created through verbs. The listener's thread
 * calls it, so what it keeps is behind a lock, and outside the object, so
 * the const methods can change it. */
class FakeRdmaCm final : public hipObj::RdmaCm {
public:
    /* device and verbs have to outlive it */
    FakeRdmaCm(FakeDevice &device, const hipObj::Verbs &verbs);
    ~FakeRdmaCm() override;

    FakeRdmaCm(const FakeRdmaCm &)            = delete;
    FakeRdmaCm &operator=(const FakeRdmaCm &) = delete;
    FakeRdmaCm(FakeRdmaCm &&)                 = delete;
    FakeRdmaCm &operator=(FakeRdmaCm &&)      = delete;

    [[nodiscard]] rdma_event_channel *create_event_channel() const override;
    void                              destroy_event_channel(rdma_event_channel *channel) const override;
    [[nodiscard]] int                 create_id(rdma_event_channel *channel, rdma_cm_id **id, void *context,
                                                rdma_port_space ps) const override;
    [[nodiscard]] int                 destroy_id(rdma_cm_id *id) const override;
    [[nodiscard]] int                 bind_addr(rdma_cm_id *id, sockaddr *addr) const override;
    [[nodiscard]] int                 listen(rdma_cm_id *id, int backlog) const override;
    [[nodiscard]] int get_cm_event(rdma_event_channel *channel, rdma_cm_event **event) const override;
    [[nodiscard]] int ack_cm_event(rdma_cm_event *event) const override;
    [[nodiscard]] int create_qp(rdma_cm_id *id, ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const override;
    void              destroy_qp(rdma_cm_id *id) const override;
    [[nodiscard]] int accept(rdma_cm_id *id, rdma_conn_param *conn_param) const override;
    [[nodiscard]] int reject(rdma_cm_id *id, const void *private_data,
                             uint8_t private_data_len) const override;
    [[nodiscard]] int disconnect(rdma_cm_id *id) const override;
    [[nodiscard]] const char *event_str(rdma_cm_event_type event) const override;

    /* Queues a connect request from a new id on the listening id's channel,
     * arriving on the context the bind resolved to, or with otherContext,
     * on another context of the same device. Returns the new id. */
    [[nodiscard]] rdma_cm_id *connectRequest(bool otherContext = false) const;

    /* Queues an event of type for id, on its channel */
    void queueEvent(rdma_cm_id *id, rdma_cm_event_type type) const;

    /* Makes the bound context's async fd readable, as an async event does */
    void raiseAsyncEvent() const;

    /* The address and port the last bind was given, such as "192.0.2.1"
     * and "18181" */
    [[nodiscard]] std::string boundAddress() const;
    [[nodiscard]] std::string boundPort() const;

    /* The context a bind to listenerAddress resolves to */
    [[nodiscard]] ibv_context *context() const;

private:
    struct State;

    FakeDevice            *device_;
    const hipObj::Verbs   *verbs_;
    std::unique_ptr<State> state_;
};

/* hipObjOps_t callbacks for the fake S3 server */
int         fakeSendRequest(void *ctx, const char *token, size_t tokenLen);
int         fakeRecvReply(void *ctx, char *reply, size_t *replyLen);
hipObjOps_t makeOps();

/* A fake device address. reg_mr is faked, so nothing reads or writes it. */
constexpr uintptr_t kFakeDevBufAddr = 0x10000;

inline void *
fakeDevBuf() noexcept
{
    return hipObj::addressToPointer<void *>(kFakeDevBufAddr);
}

/* A fake address one byte into fakeDevBuf(). It's computed as an integer,
 * since there's no object at the fake address to do pointer arithmetic
 * in. */
inline void *
fakeDevBufPlusOne() noexcept
{
    return hipObj::addressToPointer<void *>(kFakeDevBufAddr + 1);
}

/* Another fake device address, for a buffer that's registered through a
 * staging buffer. Tests make reg_mr fail for it, by setting
 * fake().fail.registerAddr to it. */
constexpr uintptr_t kFakeStagedBufAddr = 0x30000;

inline void *
fakeStagedBuf() noexcept
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
    explicit FakePlatform(FakeDevice &device)
        : hip_(device), verbs_(device), rdmaCm_(device, verbs_), platform_(hip_, verbs_, topology_, &rdmaCm_)
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

    [[nodiscard]] const FakeRdmaCm &rdmaCm() const noexcept
    {
        return rdmaCm_;
    }

private:
    Hip              hip_;
    Verbs            verbs_;
    EmptyNicTopology topology_{};
    FakeRdmaCm       rdmaCm_;
    hipObj::Platform platform_;
};

/* initWith() with config, platform, and options. hipObjInit() always
 * uses the system platform, so this is how a test initializes the library
 * with fakes. */
hipObjError_t initWithPlatform(const hipObjConfig_t &config, std::shared_ptr<const hipObj::Platform> platform,
                               const hipObj::Options &options = {});

/* Gives each test a FakePlatform over fake(), and resets fake(). Under v3,
 * the fake NIC's GID is kFakeNicGid, for the RC-CM listener to bind to,
 * and the fake S3 server sends successReply(). After each test, it shuts
 * the library down, and checks that the instance is gone and that nothing
 * but the fixture still holds the fakes. */
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

    [[nodiscard]] const FakeRdmaCm &rdmaCm() const noexcept
    {
        return fakes_->rdmaCm();
    }

    std::shared_ptr<FakePlatform<>> fakes_ = std::make_shared<FakePlatform<>>(fake());
};

} // namespace hipObjTest
