/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "fake-device.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "hipobj-span.h"
#include "hipobj.h"
#include "state.h"

namespace hipObjTest {

FakeDevice &
fake()
{
    static FakeDevice device;
    return device;
}

void
resetFake()
{
    /* Its atomics can't be assigned, so it's rebuilt in place */
    FakeDevice &f = fake();
    std::destroy_at(&f);
    std::construct_at(&f);
}

void
setReply(std::string_view reply)
{
    FakeDevice &f = fake();
    ASSERT_LT(reply.size(), f.reply.size());
    std::ranges::copy(reply, f.reply.begin());
    f.reply[reply.size()] = '\0';
    f.replyLen            = reply.size() + 1;
}

hipObj::RdmaToken
peerToken(hipObj::TransportType transport)
{
    hipObj::RdmaToken token{};
    token.transport = transport;
    token.qpNum     = 0x77;
    for (size_t i = 0; i < token.gid.size(); ++i) {
        token.gid[i] = static_cast<uint8_t>(0xf0 + i);
    }
    token.rkey       = 0x4321;
    token.remoteAddr = 0x900000;
    token.length     = kFakeBufSize;
    token.portNum    = 1;
    token.lid        = 0x12;
    return token;
}

void
setReplyWithPeerToken(const hipObj::RdmaToken &token)
{
    setReply(hipObj::encodeReplyWithPeerToken(200, token));
}

std::vector<ModifyQpCall>
modifyQpCalls()
{
    const FakeDevice                   &f = fake();
    const std::span<const ModifyQpCall> calls =
        std::span(f.modifyQp).first(std::min(f.modifyQpCount.load(), f.modifyQp.size()));
    return {calls.begin(), calls.end()};
}

// ---- ibverbs -------------------------------------------------------

namespace {

    std::array<struct ibv_device *, 2> g_devices = {{reinterpret_cast<struct ibv_device *>(0x1), nullptr}};

} // namespace

struct ibv_device **
FakeVerbs::get_device_list(int *num_devices) const
{
    if (device_->fail.getDeviceList) {
        return nullptr;
    }
    if (num_devices) {
        *num_devices = 1;
    }
    return g_devices.data();
}

void
FakeVerbs::free_device_list(struct ibv_device **) const
{
    ++device_->log.freeDeviceList;
}

struct ibv_context *
FakeVerbs::open_device(struct ibv_device *) const
{
    if (device_->fail.openDevice) {
        return nullptr;
    }
    ++device_->log.openDevice;
    return reinterpret_cast<struct ibv_context *>(0x2);
}

int
FakeVerbs::close_device(struct ibv_context *) const
{
    ++device_->log.closeDevice;
    return 0;
}

const char *
FakeVerbs::get_device_name(struct ibv_device *) const
{
    return "mlx5_0";
}

int
FakeVerbs::query_device(struct ibv_context *, struct ibv_device_attr *attr) const
{
    ++device_->log.queryDevice;
    if (device_->fail.queryDevice) {
        return -1;
    }
    hipObj::zeroObject(*attr);
    attr->vendor_id = device_->vendorId;
    return 0;
}

/* An empty GID table, so GID selection never looks at the device */
int
FakeVerbs::query_port(struct ibv_context *, uint8_t, struct ibv_port_attr *attr) const
{
    if (device_->fail.queryPort) {
        return -1;
    }
    if (attr) {
        hipObj::zeroObject(*attr);
    }
    return 0;
}

int
FakeVerbs::query_gid(struct ibv_context *, uint8_t, int, union ibv_gid *gid) const
{
    if (device_->fail.queryGid) {
        return -1;
    }
    if (gid) {
        hipObj::zeroObject(*gid);
    }
    return 0;
}

struct ibv_pd *
FakeVerbs::alloc_pd(struct ibv_context *) const
{
    if (device_->fail.allocPd) {
        return nullptr;
    }
    ++device_->log.allocPd;
    return reinterpret_cast<struct ibv_pd *>(0x3);
}

int
FakeVerbs::dealloc_pd(struct ibv_pd *) const
{
    ++device_->log.deallocPd;
    return 0;
}

struct ibv_mr *
FakeVerbs::reg_mr(struct ibv_pd *, void *addr, size_t, int access) const
{
    ++device_->log.registerMr;
    device_->log.mrAccess = access;
    if (device_->fail.registerAll || addr == device_->fail.registerAddr) {
        return nullptr;
    }
    auto *mr = static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
    if (mr) {
        mr->addr = addr;
        mr->rkey = 0x1234;
    }
    return mr;
}

bool
FakeVerbs::supportsDmabuf() const
{
    return false;
}

struct ibv_mr *
FakeVerbs::reg_dmabuf_mr(struct ibv_pd *, uint64_t, size_t, uint64_t, int, int) const
{
    errno = EOPNOTSUPP;
    return nullptr;
}

int
FakeVerbs::dereg_mr(struct ibv_mr *mr) const
{
    ++device_->log.deregMr;
    std::free(mr);
    return 0;
}

struct ibv_cq *
FakeVerbs::create_cq(struct ibv_context *, int, void *, struct ibv_comp_channel *, int) const
{
    if (device_->fail.createCq) {
        return nullptr;
    }
    auto *cq = static_cast<struct ibv_cq *>(std::calloc(1, sizeof(struct ibv_cq)));
    if (cq) {
        ++device_->log.createCq;
    }
    return cq;
}

int
FakeVerbs::destroy_cq(struct ibv_cq *cq) const
{
    ++device_->log.destroyCq;
    std::free(cq);
    return 0;
}

struct ibv_qp *
FakeVerbs::create_qp(struct ibv_pd *, struct ibv_qp_init_attr *) const
{
    if (device_->fail.createQp) {
        return nullptr;
    }
    auto *qp = static_cast<struct ibv_qp *>(std::calloc(1, sizeof(struct ibv_qp)));
    if (qp) {
        ++device_->log.createQp;
        qp->qp_num = device_->nextQpNum++;
    }
    return qp;
}

int
FakeVerbs::modify_qp(struct ibv_qp *, struct ibv_qp_attr *attr, int mask) const
{
    /* Each call records into its own element */
    const size_t call = device_->modifyQpCount++;
    if (call < device_->modifyQp.size()) {
        device_->modifyQp[call] = {*attr, mask};
    }
    return static_cast<int>(attr->qp_state) == device_->fail.modifyQpToState ? -1 : 0;
}

int
FakeVerbs::destroy_qp(struct ibv_qp *qp) const
{
    ++device_->log.destroyQp;
    std::free(qp);
    return 0;
}

int
FakeVerbs::poll_cq(struct ibv_cq *, int, struct ibv_wc *wc) const
{
    if (device_->pollResult <= 0) {
        return device_->pollResult;
    }
    hipObj::zeroObject(*wc);
    wc->status = device_->pollStatus;
    wc->opcode = device_->pollOpcode;
    return 1;
}

int
FakeVerbs::post_send(struct ibv_qp *, struct ibv_send_wr *, struct ibv_send_wr **) const
{
    ++device_->log.postSend;
    return 0;
}

// ---- HIP -------------------------------------------------------------

hipError_t
FakeHip::getDevice(int *device) const
{
    if (device_->getDeviceErr != hipSuccess) {
        return device_->getDeviceErr;
    }
    *device = device_->device;
    return hipSuccess;
}

hipError_t
FakeHip::getDeviceCount(int *count) const
{
    *count = device_->deviceCountErr == hipSuccess ? device_->deviceCount : 0;
    return device_->deviceCountErr;
}

hipError_t
FakeHip::deviceGetPCIBusId(char *pciBusId, int len, int) const
{
    const std::string_view busId = device_->gpuBusId;
    if (busId.empty()) {
        return hipErrorInvalidDevice;
    }
    const std::span<char> out = hipObj::spanFromPointer(pciBusId, static_cast<size_t>(len));
    if (busId.size() >= out.size()) {
        return hipErrorInvalidValue;
    }
    std::ranges::copy(busId, out.begin());
    out[busId.size()] = '\0';
    return hipSuccess;
}

hipError_t
FakeHip::hostMalloc(void **ptr, size_t size, unsigned int) const
{
    ++device_->log.hostMalloc;
    if (device_->fail.hostMalloc) {
        return hipErrorOutOfMemory;
    }
    *ptr                    = std::malloc(size);
    device_->lastHostMalloc = *ptr;
    return *ptr ? hipSuccess : hipErrorOutOfMemory;
}

hipError_t
FakeHip::hostFree(void *ptr) const
{
    ++device_->log.hostFree;
    device_->lastHostFree = ptr;
    std::free(ptr);
    return hipSuccess;
}

hipError_t
FakeHip::deviceSynchronize() const
{
    ++device_->log.deviceSynchronize;
    return device_->deviceSyncErr;
}

hipError_t
FakeHip::memcpyAsync(void *, const void *, size_t, hipMemcpyKind kind, hipStream_t) const
{
    ++device_->log.memcpy;
    device_->lastMemcpyKind = kind;
    return device_->memcpyErr;
}

hipError_t
FakeHip::eventCreate(hipEvent_t *event) const
{
    if (device_->eventCreateErr != hipSuccess) {
        return device_->eventCreateErr;
    }
    ++device_->log.eventCreate;
    device_->eventQueriesAtCreate = device_->log.eventQuery.load();
    *event                        = fakeEvent();
    return hipSuccess;
}

hipError_t
FakeHip::eventRecord(hipEvent_t event, hipStream_t) const
{
    EXPECT_EQ(event, fakeEvent());
    ++device_->log.eventRecord;
    return device_->eventRecordErr;
}

hipError_t
FakeHip::eventQuery(hipEvent_t event) const
{
    EXPECT_EQ(event, fakeEvent());
    const int queries = ++device_->log.eventQuery - device_->eventQueriesAtCreate;
    if (device_->eventNeverCompletes || queries <= device_->eventNotReadyCount) {
        return hipErrorNotReady;
    }
    return device_->eventQueryResult;
}

hipError_t
FakeHip::eventDestroy(hipEvent_t event) const
{
    EXPECT_EQ(event, fakeEvent());
    ++device_->log.eventDestroy;
    return hipSuccess;
}

hipError_t
FakeHip::pointerGetAttributes(hipPointerAttribute_t *attributes, const void *) const
{
    *attributes      = {};
    attributes->type = device_->memoryType;
    return hipSuccess;
}

hipError_t
FakeHip::memGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t) const
{
    ++device_->addressRangeCalls;
    if (device_->addressRangeErr != hipSuccess) {
        return device_->addressRangeErr;
    }
    *base = reinterpret_cast<hipDeviceptr_t>(device_->allocBase);
    *size = device_->allocSize;
    return hipSuccess;
}

// ---- HSA -------------------------------------------------------------

int
FakeHip::exportDmabuf(const void *, size_t, int *fd, uint64_t *offset) const
{
    ++device_->log.exportDmabuf;
    if (device_->fail.exportDmabuf) {
        /* Any nonzero hsa_status_t */
        return 1;
    }
    *fd     = kFakeDmabufFd;
    *offset = kFakeDmabufOffset;
    return 0;
}

int
FakeHip::closeDmabuf(int fd) const
{
    ++device_->log.closeDmabuf;
    device_->closedDmabufFd = fd;
    return 0;
}

// ---- S3 callbacks ----------------------------------------------------

int
fakeSendRequest(void *, const char *, size_t)
{
    ++fake().log.sendRequest;
    return 0;
}

int
fakeRecvReply(void *, char *reply, size_t *replyLen)
{
    FakeDevice &f = fake();
    ++f.log.recvReply;
    if (!replyLen || *replyLen < f.replyLen) {
        return -1;
    }
    std::ranges::copy(std::span(f.reply).first(f.replyLen),
                      hipObj::spanFromPointer(reply, *replyLen).begin());
    *replyLen = f.replyLen;
    return 0;
}

hipObjOps_t
makeOps()
{
    hipObjOps_t ops{};
    ops.sendRequest = &fakeSendRequest;
    ops.recvReply   = &fakeRecvReply;
    return ops;
}

std::vector<hipObj::NicInfo>
EmptyNicTopology::enumerate(const char *) const
{
    return {};
}

int
EmptyNicTopology::roceVersion(const char *, uint8_t, int) const
{
    return -1;
}

// ---- Platform --------------------------------------------------------

hipObjError_t
initWithPlatform(const hipObjConfig_t &config, std::shared_ptr<const hipObj::Platform> platform,
                 const hipObj::Options &options)
{
    const hipObj::ApiGuard guard;
    return hipObj::initWith(config, std::move(platform), options);
}

// ---- Fixture ---------------------------------------------------------

void
FakeDeviceTest::SetUp()
{
    resetFake();
}

void
FakeDeviceTest::TearDown()
{
    EXPECT_EQ(hipObjShutdown().opError, hipObjSuccess);
    EXPECT_EQ(hipObj::currentInstance(), nullptr);
    /* An instance that outlived its shutdown would still hold them */
    EXPECT_EQ(fakes_.use_count(), 1);
}

hipObjConfig_t
FakeDeviceTest::makeConfig()
{
    hipObjConfig_t config{};
    config.gpuDevice = -1;
    config.nicHint   = "mlx5_0";
    return config;
}

hipObjError_t
FakeDeviceTest::initFakes(const hipObjConfig_t &config, const hipObj::Options &options) const
{
    return initWithPlatform(config, fakes_->platform(), options);
}

void
FakeDeviceTest::init() const
{
    ASSERT_EQ(initFakes(makeConfig()).opError, hipObjSuccess);
}

void
FakeDeviceTest::initAndRegister() const
{
    ASSERT_NO_FATAL_FAILURE(init());
    ASSERT_EQ(hipObjBufRegister(fakeDevBuf(), kFakeBufSize).opError, hipObjSuccess);
}

} // namespace hipObjTest
