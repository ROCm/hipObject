/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * A fake RDMA device and HIP runtime for tests of the public API. The
 * fixture substitutes fakes for libibverbs, the HIP runtime, and the NIC
 * topology, so hipObjInit() and transfers run without GPU or RDMA
 * hardware. The fakes record their calls and can be
 * told to fail (see FakeDevice).
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "fake-ibv.h"
#include "hip-seam.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
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
    std::atomic<int> allocPd{0};
    std::atomic<int> deallocPd{0};
    std::atomic<int> createCq{0};
    std::atomic<int> destroyCq{0};
    std::atomic<int> createQp{0};
    std::atomic<int> destroyQp{0};
    std::atomic<int> postSend{0};

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
     * hipErrorNotReady forever. */
    hipError_t                 memcpyErr = hipSuccess;
    std::atomic<hipMemcpyKind> lastMemcpyKind{hipMemcpyDefault};
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
    return reinterpret_cast<hipEvent_t>(0x99);
}

constexpr int      kFakeDmabufFd     = 1234;
constexpr uint64_t kFakeDmabufOffset = 0x200;

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

    hipError_t getDevice(int *device) const override;
    hipError_t getDeviceCount(int *count) const override;
    hipError_t deviceGetPCIBusId(char *pciBusId, int len, int device) const override;
    hipError_t hostMalloc(void **ptr, size_t size, unsigned int flags) const override;
    hipError_t hostFree(void *ptr) const override;
    hipError_t deviceSynchronize() const override;
    hipError_t memcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind,
                           hipStream_t stream) const override;
    hipError_t eventCreate(hipEvent_t *event) const override;
    hipError_t eventRecord(hipEvent_t event, hipStream_t stream) const override;
    hipError_t eventQuery(hipEvent_t event) const override;
    hipError_t eventDestroy(hipEvent_t event) const override;
    hipError_t pointerGetAttributes(hipPointerAttribute_t *attributes, const void *ptr) const override;
    hipError_t memGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t ptr) const override;
    int        exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const override;
    int        closeDmabuf(int fd) const override;

private:
    /* A pointer, so the const methods can change what it points to */
    FakeDevice *device_;
};

/* A libibverbs that answers from a FakeDevice and records its calls there.
 * It has one device, and no ibv_reg_dmabuf_mr(). reg_mr(), create_cq(),
 * and create_qp() allocate real objects, each distinct, which dereg_mr(),
 * destroy_cq(), and destroy_qp() free; the other objects are made-up
 * addresses. A test can derive from it to change some of its methods. */
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
    struct ibv_pd *alloc_pd(struct ibv_context *context) const override;
    int            dealloc_pd(struct ibv_pd *pd) const override;
    struct ibv_mr *reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access) const override;
    bool           supportsDmabuf() const override;
    struct ibv_mr *reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd,
                                 int access) const override;
    int            dereg_mr(struct ibv_mr *mr) const override;
    struct ibv_cq *create_cq(struct ibv_context *context, int cqe, void *cq_context,
                             struct ibv_comp_channel *channel, int comp_vector) const override;
    int            destroy_cq(struct ibv_cq *cq) const override;
    struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) const override;
    int            modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask) const override;
    int            destroy_qp(struct ibv_qp *qp) const override;
    int            poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc) const override;
    int post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr) const override;

private:
    /* A pointer, so the const methods can change what it points to */
    FakeDevice *device_;
};

/* Makes hip() return runtime for as long as it lives, and puts the
 * previous runtime back when it's destroyed. runtime has to outlive it.
 * Create and destroy it only while no other thread is calling hip(). */
class ScopedFakeHip {
public:
    explicit ScopedFakeHip(const hipObj::HipRuntime &runtime)
        : runtime_(&runtime), previous_(hipObj::setHipForTest(&runtime))
    {
    }

    ~ScopedFakeHip()
    {
        /* Restored in the reverse order they were installed */
        EXPECT_EQ(hipObj::setHipForTest(previous_), runtime_);
    }

    ScopedFakeHip(const ScopedFakeHip &)            = delete;
    ScopedFakeHip &operator=(const ScopedFakeHip &) = delete;
    ScopedFakeHip(ScopedFakeHip &&)                 = delete;
    ScopedFakeHip &operator=(ScopedFakeHip &&)      = delete;

private:
    const hipObj::HipRuntime *runtime_;
    const hipObj::HipRuntime *previous_;
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
    return reinterpret_cast<void *>(kFakeDevBufAddr);
}

/* A fake address one byte into fakeDevBuf(). It's computed as an integer,
 * since there's no object at the fake address to do pointer arithmetic
 * in. */
inline void *
fakeDevBufPlusOne()
{
    return reinterpret_cast<void *>(kFakeDevBufAddr + 1);
}

constexpr size_t kFakeBufSize = 64;

/* A NIC topology with no NICs, and no RoCE version for any GID */
class EmptyNicTopology : public hipObj::NicTopology {
public:
    std::vector<hipObj::NicInfo> enumerate(const char *hca_list) const override;
    int roceVersion(const char *devName, uint8_t portNum, int gidIndex) const override;
};

/* Makes NIC and GID selection use topology for as long as it lives, and
 * puts the previous topology back when it's destroyed. topology has to
 * outlive it. Create and destroy it only while no other thread can be
 * selecting a NIC or GID. */
class ScopedFakeTopology {
public:
    explicit ScopedFakeTopology(const hipObj::NicTopology &topology)
        : topology_(&topology), previous_(hipObj::setNicTopologyForTest(&topology))
    {
    }

    ~ScopedFakeTopology()
    {
        /* Restored in the reverse order they were installed */
        EXPECT_EQ(hipObj::setNicTopologyForTest(previous_), topology_);
    }

    ScopedFakeTopology(const ScopedFakeTopology &)            = delete;
    ScopedFakeTopology &operator=(const ScopedFakeTopology &) = delete;
    ScopedFakeTopology(ScopedFakeTopology &&)                 = delete;
    ScopedFakeTopology &operator=(ScopedFakeTopology &&)      = delete;

private:
    const hipObj::NicTopology *topology_;
    const hipObj::NicTopology *previous_;
};

/* Installs the fakes for each test, and restores the real functions after.
 * After each test, it shuts the library down and checks that the instance
 * is gone. */
class FakeDeviceTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    static hipObjConfig_t makeConfig();

    /* Initializes the library with the fake device */
    static void init();

    /* Initializes and registers kFakeBufSize bytes at fakeDevBuf() */
    static void initAndRegister();

    EmptyNicTopology                  topology_{};
    std::optional<ScopedFakeTopology> fakeTopology_{};
    FakeVerbs                         verbs_{fake()};
    std::optional<ScopedFakeIbv>      fakeIbv_{};
    FakeHip                           hip_{fake()};
    std::optional<ScopedFakeHip>      fakeHip_{};
};

} // namespace hipObjTest
