/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "fake-device.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include "hipobj-address.h"
#include "hipobj-span.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "rdmacm-core.h"
#include "state.h"
#include "unique-fd.h"
#include "v3-wire.h"

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
    f.replyWithSuccess    = false;
}

void
noteSentToken([[maybe_unused]] const char *token, [[maybe_unused]] size_t tokenLen)
{
#ifdef HIPOBJ_RDMA_WIRE_V3
    const auto decoded =
        token != nullptr
            ? hipObj::v3::decodeRdmaTokenV3(hipObj::asStringView(hipObj::spanFromPointer(token, tokenLen)))
            : std::nullopt;
    fake().lastTokenLength = decoded ? decoded->length : 0;
#endif
}

std::string
successReply()
{
#ifndef HIPOBJ_RDMA_WIRE_V3
    return std::string("200");
#else
    return hipObj::v3::encodeRdmaReplyV3(
        {.httpStatus = 200, .bytesTransferred = fake().lastTokenLength.load()});
#endif
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

    /* Whether a QP can move from one state to another, for the states
     * hipObject uses. Any state can move to RESET or ERR. */
    bool isValidTransition(ibv_qp_state from, ibv_qp_state to)
    {
        if (to == IBV_QPS_RESET || to == IBV_QPS_ERR) {
            return true;
        }
        if (from == IBV_QPS_RESET) {
            return to == IBV_QPS_INIT;
        }
        if (from == IBV_QPS_INIT) {
            return to == IBV_QPS_INIT || to == IBV_QPS_RTR;
        }
        if (from == IBV_QPS_RTR || from == IBV_QPS_RTS) {
            return to == IBV_QPS_RTS;
        }
        return false;
    }

} // namespace

ibv_device **
FakeVerbs::get_device_list(int *num_devices) const
{
    if (device_->fail.getDeviceList) {
        return nullptr;
    }
    if (num_devices != nullptr) {
        *num_devices = 1;
    }
    /* Local, so it's initialized on first use rather than by a global
     * constructor */
    static std::array<ibv_device *, 2> devices = {
        {hipObj::addressToPointer<ibv_device *>(0x1), nullptr},
    };
    return devices.data();
}

void
FakeVerbs::free_device_list(ibv_device ** /*list*/) const
{
    ++device_->log.freeDeviceList;
}

ibv_context *
FakeVerbs::open_device(ibv_device * /*device*/) const
{
    if (device_->fail.openDevice) {
        return nullptr;
    }
    ++device_->log.openDevice;
    return hipObj::addressToPointer<ibv_context *>(0x2);
}

int
FakeVerbs::close_device(ibv_context * /*context*/) const
{
    ++device_->log.closeDevice;
    return 0;
}

const char *
FakeVerbs::get_device_name(ibv_device *device) const
{
    return device == fakeListenerDevice() ? device_->listenerDevice : "mlx5_0";
}

int
FakeVerbs::query_device(ibv_context * /*context*/, ibv_device_attr *attr) const
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
FakeVerbs::query_port(ibv_context * /*context*/, uint8_t /*port_num*/, ibv_port_attr *attr) const
{
    if (device_->fail.queryPort) {
        return -1;
    }
    if (attr != nullptr) {
        hipObj::zeroObject(*attr);
    }
    return 0;
}

int
FakeVerbs::query_gid(ibv_context * /*context*/, uint8_t /*port_num*/, int /*index*/, ibv_gid *gid) const
{
    if (device_->fail.queryGid) {
        return -1;
    }
    if (gid != nullptr) {
        hipObj::zeroObject(*gid);
        std::ranges::copy(device_->gid, std::span(gid->raw).begin());
    }
    return 0;
}

ibv_pd *
FakeVerbs::alloc_pd(ibv_context * /*context*/) const
{
    if (device_->fail.allocPd) {
        return nullptr;
    }
    ++device_->log.allocPd;
    return hipObj::addressToPointer<ibv_pd *>(0x3);
}

int
FakeVerbs::dealloc_pd(ibv_pd * /*pd*/) const
{
    ++device_->log.deallocPd;
    return 0;
}

ibv_mr *
FakeVerbs::reg_mr(ibv_pd * /*pd*/, void *addr, size_t /*length*/, int access) const
{
    ++device_->log.registerMr;
    device_->log.mrAccess = access;
    if (device_->fail.registerAll || addr == device_->fail.registerAddr) {
        return nullptr;
    }
    /* Released by dereg_mr() */
    auto mr  = std::make_unique<ibv_mr>();
    mr->addr = addr;
    mr->rkey = 0x1234;
    return mr.release();
}

bool
FakeVerbs::supportsDmabuf() const
{
    return false;
}

ibv_mr *
FakeVerbs::reg_dmabuf_mr(ibv_pd * /*pd*/, uint64_t /*offset*/, size_t /*length*/, uint64_t /*iova*/,
                         int /*fd*/, int /*access*/) const
{
    errno = EOPNOTSUPP;
    return nullptr;
}

int
FakeVerbs::dereg_mr(ibv_mr *mr) const
{
    ++device_->log.deregMr;
    /* From reg_mr(), or a test's fake reg_dmabuf_mr() */
    const std::unique_ptr<ibv_mr> owned(mr);
    return 0;
}

ibv_cq *
FakeVerbs::create_cq(ibv_context * /*context*/, int cqe, void * /*cq_context*/,
                     ibv_comp_channel * /*channel*/, int /*comp_vector*/) const
{
    if (device_->fail.createCq) {
        return nullptr;
    }
    /* Released by destroy_cq() */
    auto cq = std::make_unique<ibv_cq>();
    ++device_->log.createCq;
    cq->cqe = cqe;
    return cq.release();
}

int
FakeVerbs::destroy_cq(ibv_cq *cq) const
{
    ++device_->log.destroyCq;
    const std::unique_ptr<ibv_cq> owned(cq);
    return 0;
}

ibv_qp *
FakeVerbs::create_qp(ibv_pd * /*pd*/, ibv_qp_init_attr * /*qp_init_attr*/) const
{
    if (device_->fail.createQp ||
        (device_->fail.qpLimit >= 0 && device_->log.createQp >= device_->fail.qpLimit)) {
        return nullptr;
    }
    /* Zeroed, so its state is IBV_QPS_RESET. Released by destroy_qp(). */
    auto qp = std::make_unique<ibv_qp>();
    ++device_->log.createQp;
    qp->qp_num = device_->nextQpNum++;
    return qp.release();
}

int
FakeVerbs::modify_qp(ibv_qp *qp, ibv_qp_attr *attr, int mask) const
{
    /* Each call records into its own element */
    const size_t call = device_->modifyQpCount++;
    if (call < device_->modifyQp.size()) {
        device_->modifyQp[call] = {.attr = *attr, .mask = mask};
    }
    if (static_cast<int>(attr->qp_state) == device_->fail.modifyQpToState) {
        return -1;
    }
    /* Only the transfer that holds a QP changes it, so its state needs no
     * synchronization */
    if ((mask & IBV_QP_STATE) != 0) {
        if (!isValidTransition(qp->state, attr->qp_state)) {
            return -1;
        }
        qp->state = attr->qp_state;
    }
    return 0;
}

int
FakeVerbs::destroy_qp(ibv_qp *qp) const
{
    ++device_->log.destroyQp;
    const std::unique_ptr<ibv_qp> owned(qp);
    return 0;
}

int
FakeVerbs::poll_cq(ibv_cq * /*cq*/, int /*num_entries*/, ibv_wc *wc) const
{
    ++device_->log.pollCq;
    if (device_->pollResult <= 0) {
        return device_->pollResult;
    }
    hipObj::zeroObject(*wc);
    wc->status = device_->pollStatus;
    wc->opcode = device_->pollOpcode;
    return 1;
}

int
FakeVerbs::post_send(ibv_qp * /*qp*/, ibv_send_wr * /*wr*/, ibv_send_wr ** /*bad_wr*/) const
{
    ++device_->log.postSend;
    return 0;
}

int
FakeVerbs::get_async_event(ibv_context *context, ibv_async_event *event) const
{
    /* Blocks no longer than the real one's caller lets it: the listener
     * only asks after epoll reports the fd readable */
    eventfd_t count = 0;
    if (eventfd_read(context->async_fd, &count) != 0) {
        return -1;
    }
    hipObj::zeroObject(*event);
    event->event_type = device_->asyncEventType;
    return 0;
}

void
FakeVerbs::ack_async_event(ibv_async_event * /*event*/) const
{
    ++device_->log.ackAsyncEvent;
}

const char *
FakeVerbs::event_type_str(ibv_event_type /*event*/) const
{
    return "fake async event";
}

// ---- librdmacm -----------------------------------------------------

/* What FakeRdmaCm keeps. Each channel's fd counts the events queued on it
 * (EFD_SEMAPHORE, so each read takes one), and is nonblocking, so a get
 * with nothing queued fails rather than waits. */
struct FakeRdmaCm::State {
    struct Channel {
        rdma_event_channel                                      channel{};
        hipObj::UniqueFd                                        fd{};
        std::deque<std::pair<rdma_cm_id *, rdma_cm_event_type>> events{};
    };

    State() : asyncFd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE))
    {
        context.device        = fakeListenerDevice();
        context.async_fd      = asyncFd.get();
        otherContext.device   = fakeListenerDevice();
        otherContext.async_fd = -1;
    }
    ~State() = default;

    State(const State &)            = delete;
    State &operator=(const State &) = delete;
    State(State &&)                 = delete;
    State &operator=(State &&)      = delete;

    /* The channel whose rdma_event_channel is channel, or null */
    Channel *find(const rdma_event_channel *ch)
    {
        const auto it =
            std::ranges::find_if(channels, [ch](const auto &c) noexcept { return &c->channel == ch; });
        return it == channels.end() ? nullptr : it->get();
    }

    /* Queues an event for id on its channel */
    void queue(rdma_cm_id *id, rdma_cm_event_type type)
    {
        Channel *channel = find(id->channel);
        ASSERT_NE(channel, nullptr);
        channel->events.emplace_back(id, type);
        ASSERT_EQ(eventfd_write(channel->fd.get(), 1), 0);
    }

    std::mutex                                  mutex{};
    ibv_context                                 context{};
    ibv_context                                 otherContext{};
    hipObj::UniqueFd                            asyncFd;
    std::vector<std::unique_ptr<Channel>>       channels{};
    std::vector<std::unique_ptr<rdma_cm_id>>    ids{};
    std::vector<std::unique_ptr<rdma_cm_event>> events{};
    rdma_cm_id                                 *listening = nullptr;
    std::string                                 boundAddress{};
    std::string                                 boundPort{};
};

FakeRdmaCm::FakeRdmaCm(FakeDevice &device, const hipObj::Verbs &verbs)
    : device_(&device), verbs_(&verbs), state_(std::make_unique<State>())
{
}

FakeRdmaCm::~FakeRdmaCm() = default;

rdma_event_channel *
FakeRdmaCm::create_event_channel() const
{
    if (device_->fail.createEventChannel) {
        errno = ENODEV;
        return nullptr;
    }
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    auto                               channel = std::make_unique<State::Channel>();
    channel->fd            = hipObj::UniqueFd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE));
    channel->channel.fd    = channel->fd.get();
    rdma_event_channel *ch = &channel->channel;
    state_->channels.push_back(std::move(channel));
    ++device_->log.createEventChannel;
    return ch;
}

void
FakeRdmaCm::destroy_event_channel(rdma_event_channel *channel) const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    std::erase_if(state_->channels, [channel](const auto &c) noexcept { return &c->channel == channel; });
    ++device_->log.destroyEventChannel;
}

int
FakeRdmaCm::create_id(rdma_event_channel *channel, rdma_cm_id **id, void *context,
                      rdma_port_space /*ps*/) const
{
    if (device_->fail.createId) {
        errno = ENOMEM;
        return -1;
    }
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    auto                               created = std::make_unique<rdma_cm_id>();
    created->channel                           = channel;
    created->context                           = context;
    *id                                        = created.get();
    state_->ids.push_back(std::move(created));
    ++device_->log.createId;
    return 0;
}

int
FakeRdmaCm::destroy_id(rdma_cm_id *id) const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    if (state_->listening == id) {
        state_->listening = nullptr;
    }
    std::erase_if(state_->ids, [id](const auto &i) noexcept { return i.get() == id; });
    ++device_->log.destroyId;
    return 0;
}

int
FakeRdmaCm::bind_addr(rdma_cm_id *id, sockaddr *addr) const
{
    ++device_->log.bindAddr;
    if (device_->fail.bindAddr) {
        errno = EADDRINUSE;
        return -1;
    }
    const socklen_t length = addr->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> port{};
    if (getnameinfo(addr, length, host.data(), host.size(), port.data(), port.size(),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        errno = EINVAL;
        return -1;
    }
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    state_->boundAddress = hipObj::cStringView(host.data(), host.size());
    state_->boundPort    = hipObj::cStringView(port.data(), port.size());
    if (state_->boundAddress != device_->listenerAddress) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    id->verbs = device_->fail.bindResolvesNoDevice ? nullptr : &state_->context;
    return 0;
}

int
FakeRdmaCm::listen(rdma_cm_id *id, int /*backlog*/) const
{
    ++device_->log.listen;
    if (device_->fail.listen) {
        errno = EADDRINUSE;
        return -1;
    }
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    state_->listening = id;
    return 0;
}

int
FakeRdmaCm::get_cm_event(rdma_event_channel *channel, rdma_cm_event **event) const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    State::Channel                    *ch    = state_->find(channel);
    eventfd_t                          count = 0;
    if (ch == nullptr || ch->events.empty() || eventfd_read(ch->fd.get(), &count) != 0) {
        errno = EAGAIN;
        return -1;
    }
    auto created       = std::make_unique<rdma_cm_event>();
    created->id        = ch->events.front().first;
    created->listen_id = state_->listening;
    created->event     = ch->events.front().second;
    ch->events.pop_front();
    *event = created.get();
    state_->events.push_back(std::move(created));
    return 0;
}

int
FakeRdmaCm::ack_cm_event(rdma_cm_event *event) const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    std::erase_if(state_->events, [event](const auto &e) noexcept { return e.get() == event; });
    ++device_->log.ackCmEvent;
    return 0;
}

int
FakeRdmaCm::create_qp(rdma_cm_id *id, ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const
{
    if (device_->fail.cmCreateQp) {
        errno = ENOMEM;
        return -1;
    }
    ibv_qp *qp = verbs_->create_qp(pd, qp_init_attr);
    if (qp == nullptr) {
        errno = ENOMEM;
        return -1;
    }
    qp->context = id->verbs;
    qp->pd      = pd;
    qp->send_cq = qp_init_attr->send_cq;
    qp->recv_cq = qp_init_attr->recv_cq;
    id->qp      = qp;
    return 0;
}

void
FakeRdmaCm::destroy_qp(rdma_cm_id *id) const
{
    (void)verbs_->destroy_qp(id->qp);
    id->qp = nullptr;
}

int
FakeRdmaCm::accept(rdma_cm_id * /*id*/, rdma_conn_param * /*conn_param*/) const
{
    ++device_->log.accept;
    if (device_->fail.accept) {
        errno = ECONNREFUSED;
        return -1;
    }
    return 0;
}

int
FakeRdmaCm::reject(rdma_cm_id * /*id*/, const void * /*private_data*/, uint8_t /*private_data_len*/) const
{
    ++device_->log.reject;
    return 0;
}

int
FakeRdmaCm::disconnect(rdma_cm_id * /*id*/) const
{
    ++device_->log.disconnect;
    return 0;
}

const char *
FakeRdmaCm::event_str(rdma_cm_event_type /*event*/) const
{
    return "fake CM event";
}

rdma_cm_id *
FakeRdmaCm::connectRequest(bool otherContext) const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    if (state_->listening == nullptr) {
        ADD_FAILURE() << "connectRequest() without a listening id";
        return nullptr;
    }
    auto created     = std::make_unique<rdma_cm_id>();
    created->channel = state_->listening->channel;
    created->verbs   = otherContext ? &state_->otherContext : &state_->context;
    rdma_cm_id *id   = created.get();
    state_->ids.push_back(std::move(created));
    ++device_->log.createId;
    state_->queue(id, RDMA_CM_EVENT_CONNECT_REQUEST);
    return id;
}

void
FakeRdmaCm::queueEvent(rdma_cm_id *id, rdma_cm_event_type type) const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    state_->queue(id, type);
}

void
FakeRdmaCm::raiseAsyncEvent() const
{
    ASSERT_EQ(eventfd_write(state_->asyncFd.get(), 1), 0);
}

std::string
FakeRdmaCm::boundAddress() const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    return state_->boundAddress;
}

std::string
FakeRdmaCm::boundPort() const
{
    const std::scoped_lock<std::mutex> lock(state_->mutex);
    return state_->boundPort;
}

ibv_context *
FakeRdmaCm::context() const
{
    return &state_->context;
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
FakeHip::deviceGetPCIBusId(char *pciBusId, int len, int /*device*/) const
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
FakeHip::hostMalloc(void **ptr, size_t size, unsigned int /*flags*/) const
{
    ++device_->log.hostMalloc;
    if (device_->fail.hostMalloc) {
        return hipErrorOutOfMemory;
    }
    /* Raw memory, as hipHostMalloc() returns, which hostFree() or
     * freeFakeHostMemory() releases */
    *ptr                    = ::operator new(size);
    device_->lastHostMalloc = *ptr;
    return hipSuccess;
}

void
freeFakeHostMemory(void *ptr)
{
    ::operator delete(ptr);
}

hipError_t
FakeHip::hostFree(void *ptr) const
{
    ++device_->log.hostFree;
    device_->lastHostFree = ptr;
    freeFakeHostMemory(ptr);
    return hipSuccess;
}

hipError_t
FakeHip::deviceSynchronize() const
{
    ++device_->log.deviceSynchronize;
    return device_->deviceSyncErr;
}

hipError_t
FakeHip::memcpyAsync(void * /*dst*/, const void * /*src*/, size_t /*size*/, hipMemcpyKind kind,
                     hipStream_t stream) const
{
    ++device_->log.memcpy;
    device_->lastMemcpyKind   = kind;
    device_->lastMemcpyStream = stream;
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
FakeHip::eventRecord(hipEvent_t event, hipStream_t stream) const
{
    EXPECT_EQ(event, fakeEvent());
    ++device_->log.eventRecord;
    device_->lastEventRecordStream = stream;
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
FakeHip::pointerGetAttributes(hipPointerAttribute_t *attributes, const void * /*ptr*/) const
{
    *attributes      = {};
    attributes->type = device_->memoryType;
    return hipSuccess;
}

hipError_t
FakeHip::memGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t /*ptr*/) const
{
    ++device_->addressRangeCalls;
    if (device_->addressRangeErr != hipSuccess) {
        return device_->addressRangeErr;
    }
    *base = hipObj::addressToPointer<hipDeviceptr_t>(device_->allocBase);
    *size = device_->allocSize;
    return hipSuccess;
}

// ---- HSA -------------------------------------------------------------

int
FakeHip::exportDmabuf(const void * /*ptr*/, size_t /*size*/, int *fd, uint64_t *offset) const
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
fakeSendRequest(void * /*ctx*/, const char *token, size_t tokenLen)
{
    ++fake().log.sendRequest;
    noteSentToken(token, tokenLen);
    return 0;
}

int
fakeRecvReply(void * /*ctx*/, char *reply, size_t *replyLen)
{
    FakeDevice &f = fake();
    ++f.log.recvReply;
    if (f.replyWithSuccess) {
        /* With its NUL, as setReply() stores a reply */
        const std::string success = successReply();
        if (replyLen == nullptr || *replyLen < success.size() + 1) {
            return -1;
        }
        const std::span<char> out = hipObj::spanFromPointer(reply, *replyLen);
        std::ranges::copy(success, out.begin());
        out[success.size()] = '\0';
        *replyLen           = success.size() + 1;
        return 0;
    }
    if (replyLen == nullptr || *replyLen < f.replyLen) {
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
EmptyNicTopology::enumerate(std::string_view /*hcaList*/) const
{
    return {};
}

hipObj::RoceVersion
EmptyNicTopology::roceVersion(std::string_view /*devName*/, uint8_t /*portNum*/, int /*gidIndex*/) const
{
    return hipObj::RoceVersion::Unknown;
}

// ---- Platform --------------------------------------------------------

hipObjError_t
initWithPlatform(const hipObjConfig_t &config, std::shared_ptr<const hipObj::Platform> platform,
                 const hipObj::Options &options)
{
    return hipObj::initWith(config, std::move(platform), options);
}

// ---- Fixture ---------------------------------------------------------

void
FakeDeviceTest::SetUp()
{
    resetFake();
#ifdef HIPOBJ_RDMA_WIRE_V3
    /* FakeDevice's default reply, "200", is a v1 reply. hipObject is built
     * for one wire format (see useV1Wire() in state.h), so under v3 the
     * success reply is the encoder's own, which can't drift from the format,
     * and reports the transfer's length (see successReply()). */
    fake().replyWithSuccess = true;
    /* v3's listener binds to the NIC's address */
    fake().gid = kFakeNicGid;
#endif
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
