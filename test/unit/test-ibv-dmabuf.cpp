/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * registerDeviceMemory()'s dma-buf path, with the HSA export and close (in
 * the fake HIP runtime) and the libibverbs registration faked, so no GPU or
 * NIC is needed.
 *
 * The DmabufDisabledTest tests need HIPOBJ_DMABUF=0 in the environment the
 * program starts with, so they skip unless it's set, and CMakeLists.txt
 * runs this program a second time, with it set, for just those tests. The
 * DmabufEnvTest tests skip when HIPOBJ_DMABUF turns dma-buf registration
 * off.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "buffer.h"
#include "fake-device.h"
#include "fake-ibv.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-ptr.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;
using hipObjTest::fakeDevBuf;
using hipObjTest::kFakeDmabufFd;
using hipObjTest::kFakeDmabufOffset;

constexpr size_t kBufSize = 64;
constexpr int    kAccess  = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

/* The fake ibv_reg_dmabuf_mr()'s calls. The fake HIP runtime records the
 * export and close, and the fake verbs record the plain registrations, in
 * fake(). */
struct DmabufLog {
    bool failRegister = false;

    int      dmabufRegisterCalls = 0;
    int      registeredFd        = -1;
    uint64_t registeredOffset    = 0;
    uint64_t registeredIova      = 0;
    /* fake().log.closeDmabuf when the fake ibv_reg_dmabuf_mr() ran */
    int closeCallsDuringRegister = -1;
};

DmabufLog g_log;

struct ibv_pd *
fakePd()
{
    return reinterpret_cast<struct ibv_pd *>(1);
}

/* Returns false when HIPOBJ_DMABUF turns dma-buf registration off, by the
 * same test as the library's */
bool
dmabufEnabledByEnv()
{
    const char *env = std::getenv("HIPOBJ_DMABUF");
    return !(env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N'));
}

/* A libibverbs with ibv_reg_dmabuf_mr() */
class DmabufVerbs : public hipObjTest::FakeVerbs {
public:
    using FakeVerbs::FakeVerbs;

    bool supportsDmabuf() const override
    {
        return true;
    }

    /* Leaves the MR's addr NULL, as the real ibv_reg_dmabuf_mr() does,
     * which also tells the tests which path registered it */
    struct ibv_mr *reg_dmabuf_mr(struct ibv_pd *, uint64_t offset, size_t, uint64_t iova, int fd,
                                 int) const override
    {
        ++g_log.dmabufRegisterCalls;
        g_log.registeredFd             = fd;
        g_log.registeredOffset         = offset;
        g_log.registeredIova           = iova;
        g_log.closeCallsDuringRegister = fake().log.closeDmabuf;
        if (g_log.failRegister) {
            errno = EINVAL;
            return nullptr;
        }
        return static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
    }
};

/* Installs the fakes for each test, and removes them after. The MRs the
 * tests get back deregister through ibv(), so it's the fake verbs too. */
class DmabufTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fakeIbv_.emplace(verbs_);
        fakeHip_.emplace(hip_);

        g_log  = {};
        fake() = hipObjTest::FakeDevice{};
    }

    void TearDown() override
    {
        fakeIbv_.reset();
        fakeHip_.reset();
    }

    hipObj::IbvMrPtr registerFakeBuffer(const hipObj::DmabufPolicy &policy = {}) const
    {
        return hipObj::registerDeviceMemory(verbs_, hip_, fakePd(), fakeDevBuf(), kBufSize, kAccess, policy);
    }

    DmabufVerbs                              verbs_{fake()};
    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_{};
    hipObjTest::FakeHip                      hip_{fake()};
    std::optional<hipObjTest::ScopedFakeHip> fakeHip_{};
};

TEST_F(DmabufTest, RegistrationClosesFdBeforeReturning)
{
    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, nullptr);
    EXPECT_EQ(fake().log.exportDmabuf, 1);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 1);
    EXPECT_EQ(g_log.registeredFd, kFakeDmabufFd);
    EXPECT_EQ(g_log.registeredOffset, kFakeDmabufOffset);
    EXPECT_EQ(g_log.registeredIova, reinterpret_cast<uintptr_t>(fakeDevBuf()));
    EXPECT_EQ(fake().log.registerMr, 0);

    /* Open while it was registered, and closed once by the time
     * registerDeviceMemory() returned */
    EXPECT_EQ(g_log.closeCallsDuringRegister, 0);
    EXPECT_EQ(fake().log.closeDmabuf, 1);
    EXPECT_EQ(fake().closedDmabufFd, kFakeDmabufFd);

    mr.reset();
    EXPECT_EQ(fake().log.deregMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 1);
}

TEST_F(DmabufTest, FailedRegistrationClosesFdAndFallsBack)
{
    g_log.failRegister = true;

    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(g_log.dmabufRegisterCalls, 1);
    EXPECT_EQ(fake().log.registerMr, 1);

    EXPECT_EQ(g_log.closeCallsDuringRegister, 0);
    EXPECT_EQ(fake().log.closeDmabuf, 1);
    EXPECT_EQ(fake().closedDmabufFd, kFakeDmabufFd);

    mr.reset();
    EXPECT_EQ(fake().log.deregMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 1);
}

TEST_F(DmabufTest, FailedExportClosesNothing)
{
    fake().fail.exportDmabuf = true;

    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(fake().log.exportDmabuf, 1);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 0);
    EXPECT_EQ(fake().log.registerMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 0);
}

/* The debug policy only adds a message */
TEST_F(DmabufTest, FailedExportWithDebugFallsBack)
{
    fake().fail.exportDmabuf = true;

    hipObj::IbvMrPtr mr = registerFakeBuffer({.enabled = true, .debug = true});
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(fake().log.exportDmabuf, 1);
    EXPECT_EQ(fake().log.registerMr, 1);
}

/* Nothing registers when the plain registration fails too */
TEST_F(DmabufTest, FailedFallbackRegistersNothing)
{
    g_log.failRegister        = true;
    fake().fail.registerAll   = true;
    const hipObj::IbvMrPtr mr = registerFakeBuffer();
    EXPECT_EQ(mr, nullptr);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 1);
    EXPECT_EQ(fake().log.registerMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 1);
}

/* libibverbs older than rdma-core 34 has no ibv_reg_dmabuf_mr() */
TEST_F(DmabufTest, NoDmabufVerbSkipsExport)
{
    const hipObjTest::FakeVerbs plain(fake());

    hipObj::IbvMrPtr mr =
        hipObj::registerDeviceMemory(plain, hip_, fakePd(), fakeDevBuf(), kBufSize, kAccess, {});
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(fake().log.exportDmabuf, 0);
    EXPECT_EQ(fake().log.registerMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 0);
}

TEST_F(DmabufTest, DisabledPolicyRegistersWithoutExporting)
{
    hipObj::IbvMrPtr mr = registerFakeBuffer({.enabled = false, .debug = false});
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(fake().log.exportDmabuf, 0);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 0);
    EXPECT_EQ(fake().log.registerMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 0);
}

// ---- The policy read from the environment ------------------------

/* Registers a buffer through BufferMap, which takes its policy from the
 * environment */
class DmabufEnvTest : public DmabufTest {
protected:
    void SetUp() override
    {
        DmabufTest::SetUp();
        if (!dmabufEnabledByEnv()) {
            GTEST_SKIP() << "HIPOBJ_DMABUF turns dma-buf registration off";
        }
    }
};

class DmabufDisabledTest : public DmabufTest {
protected:
    void SetUp() override
    {
        DmabufTest::SetUp();
        if (dmabufEnabledByEnv()) {
            GTEST_SKIP() << "needs HIPOBJ_DMABUF=0";
        }
    }
};

TEST_F(DmabufEnvTest, BufferMapExportsByDefault)
{
    hipObj::BufferMap buffers;
    ASSERT_EQ(buffers.registerBuffer(fakeDevBuf(), kBufSize, fakePd()), 0);
    EXPECT_EQ(fake().log.exportDmabuf, 1);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 1);
    EXPECT_EQ(fake().log.registerMr, 0);
}

TEST_F(DmabufDisabledTest, BufferMapRegistersWithoutExporting)
{
    hipObj::BufferMap buffers;
    ASSERT_EQ(buffers.registerBuffer(fakeDevBuf(), kBufSize, fakePd()), 0);
    EXPECT_EQ(fake().log.exportDmabuf, 0);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 0);
    EXPECT_EQ(fake().log.registerMr, 1);
    EXPECT_EQ(fake().log.closeDmabuf, 0);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
