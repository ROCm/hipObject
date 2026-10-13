/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * LoadedVerbs, the production Verbs, built from fake libibverbs functions
 * that record what they were called with, so no RDMA device is needed.
 * Every other test replaces it with fake verbs, so this is the only test
 * of how it forwards each call.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* The last call a fake function got: its name, the object it was given
 * (its first pointer argument), and one other argument */
struct Call {
    std::string_view name   = {};
    const void      *object = nullptr;
    uint64_t         value  = 0;
};

Call g_call;

/* What the fake functions return */
template <typename T>
T *
fakeObject(uintptr_t address)
{
    return reinterpret_cast<T *>(address);
}

constexpr int kResult = 42;

/* The fakes for every function, ibv_reg_dmabuf_mr() and the inline
 * functions' symbols included */
hipObj::LoadedVerbs::Funcs
makeFuncs()
{
    hipObj::LoadedVerbs::Funcs funcs{};
    funcs.get_device_list = [](int *num) {
        g_call = {"get_device_list", num, 0};
        *num   = 1;
        return fakeObject<struct ibv_device *>(0x10);
    };
    funcs.free_device_list = [](struct ibv_device **list) {
        g_call = {"free_device_list", static_cast<const void *>(list), 0};
    };
    funcs.open_device = [](struct ibv_device *device) {
        g_call = {"open_device", device, 0};
        return fakeObject<struct ibv_context>(0x20);
    };
    funcs.close_device = [](struct ibv_context *context) {
        g_call = {"close_device", context, 0};
        return kResult;
    };
    funcs.get_device_name = [](struct ibv_device *device) {
        g_call = {"get_device_name", device, 0};
        return "fake0";
    };
    funcs.query_device = [](struct ibv_context *context, struct ibv_device_attr *attr) {
        g_call = {"query_device", context, reinterpret_cast<uintptr_t>(attr)};
        return kResult;
    };
    funcs.query_port = [](struct ibv_context *context, uint8_t port, struct ibv_port_attr *) {
        g_call = {"query_port", context, port};
        return kResult;
    };
    funcs.query_gid = [](struct ibv_context *context, uint8_t, int index, union ibv_gid *) {
        g_call = {"query_gid", context, static_cast<uint64_t>(index)};
        return kResult;
    };
    funcs.alloc_pd = [](struct ibv_context *context) {
        g_call = {"alloc_pd", context, 0};
        return fakeObject<struct ibv_pd>(0x30);
    };
    funcs.dealloc_pd = [](struct ibv_pd *pd) {
        g_call = {"dealloc_pd", pd, 0};
        return kResult;
    };
    funcs.reg_dmabuf_mr = [](struct ibv_pd *pd, uint64_t, size_t, uint64_t iova, int, int) {
        g_call = {"reg_dmabuf_mr", pd, iova};
        return fakeObject<struct ibv_mr>(0x40);
    };
    funcs.reg_mr_iova2 = [](struct ibv_pd *pd, void *, size_t, uintptr_t iova, int) {
        g_call = {"reg_mr_iova2", pd, iova};
        return fakeObject<struct ibv_mr>(0x50);
    };
    funcs.dereg_mr = [](struct ibv_mr *mr) {
        g_call = {"dereg_mr", mr, 0};
        return kResult;
    };
    funcs.create_cq = [](struct ibv_context *context, int cqe, void *, struct ibv_comp_channel *, int) {
        g_call = {"create_cq", context, static_cast<uint64_t>(cqe)};
        return fakeObject<struct ibv_cq>(0x60);
    };
    funcs.destroy_cq = [](struct ibv_cq *cq) {
        g_call = {"destroy_cq", cq, 0};
        return kResult;
    };
    funcs.create_qp = [](struct ibv_pd *pd, struct ibv_qp_init_attr *) {
        g_call = {"create_qp", pd, 0};
        return fakeObject<struct ibv_qp>(0x70);
    };
    funcs.modify_qp = [](struct ibv_qp *qp, struct ibv_qp_attr *, int mask) {
        g_call = {"modify_qp", qp, static_cast<uint64_t>(mask)};
        return kResult;
    };
    funcs.destroy_qp = [](struct ibv_qp *qp) {
        g_call = {"destroy_qp", qp, 0};
        return kResult;
    };
    funcs.poll_cq = [](struct ibv_cq *cq, int count, struct ibv_wc *) {
        g_call = {"poll_cq", cq, static_cast<uint64_t>(count)};
        return kResult;
    };
    funcs.post_send = [](struct ibv_qp *qp, struct ibv_send_wr *, struct ibv_send_wr **) {
        g_call = {"post_send", qp, 0};
        return kResult;
    };
    funcs.post_recv = [](struct ibv_qp *qp, struct ibv_recv_wr *, struct ibv_recv_wr **) {
        g_call = {"post_recv", qp, 0};
        return kResult;
    };
    return funcs;
}

class LoadedVerbsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        g_call = {};
    }

    /* Checks that the last call was to name, with object */
    static void expectCall(std::string_view name, const void *object)
    {
        EXPECT_EQ(g_call.name, name);
        EXPECT_EQ(g_call.object, object);
    }
};

TEST_F(LoadedVerbsTest, ForwardsEachCall)
{
    const hipObj::LoadedVerbs verbs(makeFuncs());

    int num = 0;
    EXPECT_EQ(verbs.get_device_list(&num), fakeObject<struct ibv_device *>(0x10));
    expectCall("get_device_list", &num);
    EXPECT_EQ(num, 1);
    auto *const devices = fakeObject<struct ibv_device *>(0x11);
    verbs.free_device_list(devices);
    expectCall("free_device_list", static_cast<const void *>(devices));

    auto *const device = fakeObject<struct ibv_device>(0x12);
    EXPECT_EQ(verbs.open_device(device), fakeObject<struct ibv_context>(0x20));
    expectCall("open_device", device);
    EXPECT_EQ(std::string_view(verbs.get_device_name(device)), "fake0");
    expectCall("get_device_name", device);

    auto *const     context = fakeObject<struct ibv_context>(0x21);
    ibv_device_attr deviceAttr{};
    EXPECT_EQ(verbs.query_device(context, &deviceAttr), kResult);
    expectCall("query_device", context);
    EXPECT_EQ(g_call.value, reinterpret_cast<uintptr_t>(&deviceAttr));
    ibv_port_attr portAttr{};
    EXPECT_EQ(verbs.query_port(context, 3, &portAttr), kResult);
    expectCall("query_port", context);
    EXPECT_EQ(g_call.value, 3U);
    ibv_gid gid{};
    EXPECT_EQ(verbs.query_gid(context, 1, 5, &gid), kResult);
    expectCall("query_gid", context);
    EXPECT_EQ(g_call.value, 5U);
    EXPECT_EQ(verbs.alloc_pd(context), fakeObject<struct ibv_pd>(0x30));
    expectCall("alloc_pd", context);
    EXPECT_EQ(verbs.create_cq(context, 16, nullptr, nullptr, 0), fakeObject<struct ibv_cq>(0x60));
    expectCall("create_cq", context);
    EXPECT_EQ(g_call.value, 16U);
    EXPECT_EQ(verbs.close_device(context), kResult);
    expectCall("close_device", context);

    auto *const      pd = fakeObject<struct ibv_pd>(0x31);
    ibv_qp_init_attr initAttr{};
    EXPECT_EQ(verbs.create_qp(pd, &initAttr), fakeObject<struct ibv_qp>(0x70));
    expectCall("create_qp", pd);
    EXPECT_EQ(verbs.dealloc_pd(pd), kResult);
    expectCall("dealloc_pd", pd);

    auto *const mr = fakeObject<struct ibv_mr>(0x41);
    EXPECT_EQ(verbs.dereg_mr(mr), kResult);
    expectCall("dereg_mr", mr);

    auto *const cq = fakeObject<struct ibv_cq>(0x61);
    ibv_wc      wc{};
    EXPECT_EQ(verbs.poll_cq(cq, 1, &wc), kResult);
    expectCall("poll_cq", cq);
    EXPECT_EQ(g_call.value, 1U);
    EXPECT_EQ(verbs.destroy_cq(cq), kResult);
    expectCall("destroy_cq", cq);

    auto *const  qp = fakeObject<struct ibv_qp>(0x71);
    ibv_qp_attr  qpAttr{};
    ibv_send_wr  sendWr{};
    ibv_send_wr *badSend = nullptr;
    ibv_recv_wr  recvWr{};
    ibv_recv_wr *badRecv = nullptr;
    EXPECT_EQ(verbs.modify_qp(qp, &qpAttr, IBV_QP_STATE), kResult);
    expectCall("modify_qp", qp);
    EXPECT_EQ(g_call.value, static_cast<uint64_t>(IBV_QP_STATE));
    EXPECT_EQ(verbs.post_send(qp, &sendWr, &badSend), kResult);
    expectCall("post_send", qp);
    EXPECT_EQ(verbs.post_recv(qp, &recvWr, &badRecv), kResult);
    expectCall("post_recv", qp);
    EXPECT_EQ(verbs.destroy_qp(qp), kResult);
    expectCall("destroy_qp", qp);
}

/* ibv_reg_mr() registers with the address as the IOVA */
TEST_F(LoadedVerbsTest, RegMrUsesAddressAsIova)
{
    const hipObj::LoadedVerbs verbs(makeFuncs());
    auto *const               pd   = fakeObject<struct ibv_pd>(0x31);
    void *const               addr = fakeObject<void>(0x1000);

    EXPECT_EQ(verbs.reg_mr(pd, addr, 64, IBV_ACCESS_LOCAL_WRITE), fakeObject<struct ibv_mr>(0x50));
    expectCall("reg_mr_iova2", pd);
    EXPECT_EQ(g_call.value, 0x1000U);
}

TEST_F(LoadedVerbsTest, RegistersDmabufWhenLibibverbsCan)
{
    const hipObj::LoadedVerbs verbs(makeFuncs());
    auto *const               pd = fakeObject<struct ibv_pd>(0x31);

    EXPECT_TRUE(verbs.supportsDmabuf());
    EXPECT_EQ(verbs.reg_dmabuf_mr(pd, 0, 64, 0x2000, 7, IBV_ACCESS_LOCAL_WRITE),
              fakeObject<struct ibv_mr>(0x40));
    expectCall("reg_dmabuf_mr", pd);
    EXPECT_EQ(g_call.value, 0x2000U);
}

/* libibverbs older than rdma-core 34 has no ibv_reg_dmabuf_mr() */
TEST_F(LoadedVerbsTest, NoDmabufWithoutTheSymbol)
{
    hipObj::LoadedVerbs::Funcs funcs = makeFuncs();
    funcs.reg_dmabuf_mr              = nullptr;
    const hipObj::LoadedVerbs verbs(funcs);

    EXPECT_FALSE(verbs.supportsDmabuf());
    errno = 0;
    EXPECT_EQ(verbs.reg_dmabuf_mr(fakeObject<struct ibv_pd>(0x31), 0, 64, 0x2000, 7, IBV_ACCESS_LOCAL_WRITE),
              nullptr);
    EXPECT_EQ(errno, EOPNOTSUPP);
    EXPECT_EQ(g_call.name, "");
}

/* Without the symbols for the inline functions in <infiniband/verbs.h>,
 * the calls go to the provider's entries in the context's ops, as the
 * inline functions do */
TEST_F(LoadedVerbsTest, FallsBackToContextOps)
{
    hipObj::LoadedVerbs::Funcs funcs = makeFuncs();
    funcs.poll_cq                    = nullptr;
    funcs.post_send                  = nullptr;
    funcs.post_recv                  = nullptr;
    const hipObj::LoadedVerbs verbs(funcs);

    struct ibv_context context {};
    context.ops.poll_cq = [](struct ibv_cq *cq, int count, struct ibv_wc *) {
        g_call = {"ops.poll_cq", cq, static_cast<uint64_t>(count)};
        return kResult;
    };
    context.ops.post_send = [](struct ibv_qp *qp, struct ibv_send_wr *, struct ibv_send_wr **) {
        g_call = {"ops.post_send", qp, 0};
        return kResult;
    };
    context.ops.post_recv = [](struct ibv_qp *qp, struct ibv_recv_wr *, struct ibv_recv_wr **) {
        g_call = {"ops.post_recv", qp, 0};
        return kResult;
    };
    struct ibv_cq cq {};
    cq.context = &context;
    struct ibv_qp qp {};
    qp.context = &context;

    ibv_wc wc{};
    EXPECT_EQ(verbs.poll_cq(&cq, 2, &wc), kResult);
    expectCall("ops.poll_cq", &cq);
    EXPECT_EQ(g_call.value, 2U);

    ibv_send_wr  sendWr{};
    ibv_send_wr *badSend = nullptr;
    EXPECT_EQ(verbs.post_send(&qp, &sendWr, &badSend), kResult);
    expectCall("ops.post_send", &qp);

    ibv_recv_wr  recvWr{};
    ibv_recv_wr *badRecv = nullptr;
    EXPECT_EQ(verbs.post_recv(&qp, &recvWr, &badRecv), kResult);
    expectCall("ops.post_recv", &qp);
}

/* The real libibverbs, where it's installed. Listing the devices works
 * without any. */
TEST(LoadLibibverbsTest, LoadsWhatTheLibraryNeeds)
{
    const std::unique_ptr<hipObj::LoadedVerbs> verbs = hipObj::loadLibibverbs();
    if (!verbs) {
        GTEST_SKIP() << "libibverbs couldn't be loaded";
    }

    int                 num  = -1;
    struct ibv_device **list = verbs->get_device_list(&num);
    if (list) {
        EXPECT_GE(num, 0);
        verbs->free_device_list(list);
    }
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
