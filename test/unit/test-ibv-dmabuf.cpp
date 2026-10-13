/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * IBVWrapper::reg_mr()'s dma-buf path, with the HSA export and close and
 * the libibverbs registration faked, so no GPU or NIC is needed.
 *
 * The DmabufDisabledTest tests need HIPOBJ_DMABUF=0 in the environment the
 * program starts with, so they skip unless it's set, and CMakeLists.txt
 * runs this program a second time, with it set, for just those tests. The
 * DmabufTest tests skip when HIPOBJ_DMABUF turns dma-buf registration off.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <hsa/hsa.h>

#include "fake-ibv.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

constexpr int      kFakeFd     = 1234;
constexpr uint64_t kFakeOffset = 0x200;
constexpr size_t   kBufSize    = 64;
constexpr int      kAccess     = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

struct DmabufLog {
    bool failExport   = false;
    bool failRegister = false;

    int exportCalls = 0;
    int closeCalls  = 0;
    int closedFd    = -1;

    int      dmabufRegisterCalls = 0;
    int      registeredFd        = -1;
    uint64_t registeredOffset    = 0;
    uint64_t registeredIova      = 0;
    /* closeCalls when the fake ibv_reg_dmabuf_mr() ran */
    int closeCallsDuringRegister = -1;

    int plainRegisterCalls = 0;
    int deregisterCalls    = 0;
};

DmabufLog g_log;

/* A fake device address. Everything that would use it is faked. */
void *
fakeDevBuf()
{
    return reinterpret_cast<void *>(0x10000);
}

struct ibv_pd *
fakePd()
{
    return reinterpret_cast<struct ibv_pd *>(1);
}

/* Returns false when HIPOBJ_DMABUF turns dma-buf registration off, by the
 * same test as IBVWrapper::init_dmabuf_settings() */
bool
dmabufEnabledByEnv()
{
    const char *env = std::getenv("HIPOBJ_DMABUF");
    return !(env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N'));
}

int
fakeExportDmabuf(const void *, size_t, int *fd, uint64_t *offset)
{
    ++g_log.exportCalls;
    if (g_log.failExport) {
        return static_cast<int>(HSA_STATUS_ERROR_INVALID_ALLOCATION);
    }
    *fd     = kFakeFd;
    *offset = kFakeOffset;
    return 0;
}

int
fakeCloseDmabuf(int fd)
{
    ++g_log.closeCalls;
    g_log.closedFd = fd;
    return 0;
}

struct ibv_mr *
makeFakeMr(void *addr)
{
    auto *mr = static_cast<struct ibv_mr *>(std::calloc(1, sizeof(struct ibv_mr)));
    if (mr) {
        mr->addr = addr;
        mr->rkey = 0x1234;
    }
    return mr;
}

/* Leaves the MR's addr NULL, as the real ibv_reg_dmabuf_mr() does, which
 * also tells the tests which path registered it */
struct ibv_mr *
fakeRegisterDmabufMr(struct ibv_pd *, uint64_t offset, size_t, uint64_t iova, int fd, int)
{
    ++g_log.dmabufRegisterCalls;
    g_log.registeredFd             = fd;
    g_log.registeredOffset         = offset;
    g_log.registeredIova           = iova;
    g_log.closeCallsDuringRegister = g_log.closeCalls;
    if (g_log.failRegister) {
        errno = EINVAL;
        return nullptr;
    }
    return makeFakeMr(nullptr);
}

struct ibv_mr *
fakeRegisterMr(struct ibv_pd *, void *addr, size_t, int)
{
    ++g_log.plainRegisterCalls;
    return makeFakeMr(addr);
}

struct ibv_mr *
fakeRegisterMrIova2(struct ibv_pd *, void *addr, size_t, uintptr_t, int)
{
    return fakeRegisterMr(nullptr, addr, 0, 0);
}

int
fakeDeregisterMr(struct ibv_mr *mr)
{
    ++g_log.deregisterCalls;
    std::free(mr);
    return 0;
}

/* Installs the fakes for each test, and removes them after */
class FakeDmabufTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        hipObj::IbvFuncs funcs = {};
        funcs.export_dmabuf    = &fakeExportDmabuf;
        funcs.close_dmabuf     = &fakeCloseDmabuf;
        funcs.reg_dmabuf_mr    = &fakeRegisterDmabufMr;
        funcs.reg_mr_iova2     = &fakeRegisterMrIova2;
        funcs.dereg_mr         = &fakeDeregisterMr;
        fakeIbv_.emplace(funcs);

        g_log = {};
    }

    void TearDown() override
    {
        fakeIbv_.reset();
    }

    static hipObj::IbvMrPtr registerFakeBuffer()
    {
        return hipObj::IbvMrPtr(hipObj::ibv().reg_mr(fakePd(), fakeDevBuf(), kBufSize, kAccess));
    }

    std::optional<hipObjTest::ScopedFakeIbv> fakeIbv_;
};

class DmabufTest : public FakeDmabufTest {
protected:
    void SetUp() override
    {
        FakeDmabufTest::SetUp();
        if (!dmabufEnabledByEnv()) {
            GTEST_SKIP() << "HIPOBJ_DMABUF turns dma-buf registration off";
        }
    }
};

TEST_F(DmabufTest, RegistrationClosesFdBeforeReturning)
{
    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, nullptr);
    EXPECT_EQ(g_log.exportCalls, 1);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 1);
    EXPECT_EQ(g_log.registeredFd, kFakeFd);
    EXPECT_EQ(g_log.registeredOffset, kFakeOffset);
    EXPECT_EQ(g_log.registeredIova, reinterpret_cast<uintptr_t>(fakeDevBuf()));
    EXPECT_EQ(g_log.plainRegisterCalls, 0);

    /* Open while it was registered, and closed once by the time reg_mr()
     * returned */
    EXPECT_EQ(g_log.closeCallsDuringRegister, 0);
    EXPECT_EQ(g_log.closeCalls, 1);
    EXPECT_EQ(g_log.closedFd, kFakeFd);

    EXPECT_EQ(hipObj::ibv().dereg_mr(mr.release()), 0);
    EXPECT_EQ(g_log.deregisterCalls, 1);
    EXPECT_EQ(g_log.closeCalls, 1);
}

TEST_F(DmabufTest, FailedRegistrationClosesFdAndFallsBack)
{
    g_log.failRegister = true;

    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(g_log.dmabufRegisterCalls, 1);
    EXPECT_EQ(g_log.plainRegisterCalls, 1);

    EXPECT_EQ(g_log.closeCallsDuringRegister, 0);
    EXPECT_EQ(g_log.closeCalls, 1);
    EXPECT_EQ(g_log.closedFd, kFakeFd);

    EXPECT_EQ(hipObj::ibv().dereg_mr(mr.release()), 0);
    EXPECT_EQ(g_log.deregisterCalls, 1);
    EXPECT_EQ(g_log.closeCalls, 1);
}

TEST_F(DmabufTest, FailedExportClosesNothing)
{
    g_log.failExport = true;

    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(g_log.exportCalls, 1);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 0);
    EXPECT_EQ(g_log.plainRegisterCalls, 1);
    EXPECT_EQ(g_log.closeCalls, 0);
}

/* libibverbs older than rdma-core 34 has no ibv_reg_dmabuf_mr() */
TEST_F(DmabufTest, NoDmabufVerbSkipsExport)
{
    hipObj::IbvFuncs funcs = fakeIbv_->funcs();
    funcs.reg_dmabuf_mr    = nullptr;
    fakeIbv_->replace(funcs);

    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(g_log.exportCalls, 0);
    EXPECT_EQ(g_log.plainRegisterCalls, 1);
    EXPECT_EQ(g_log.closeCalls, 0);
}

// ---- Settings read from the environment --------------------------

class DmabufDisabledTest : public FakeDmabufTest {
protected:
    void SetUp() override
    {
        FakeDmabufTest::SetUp();
        if (dmabufEnabledByEnv()) {
            GTEST_SKIP() << "needs HIPOBJ_DMABUF=0";
        }
    }
};

/* Sets an environment variable for its lifetime, and puts back the value
 * it had before */
class ScopedEnv {
public:
    ScopedEnv(const char *name, const char *value) : name_(name)
    {
        if (const char *previous = std::getenv(name)) {
            previous_ = previous;
        }
        EXPECT_EQ(setenv(name, value, 1), 0);
    }

    ~ScopedEnv()
    {
        if (previous_) {
            (void)setenv(name_, previous_->c_str(), 1);
        }
        else {
            (void)unsetenv(name_);
        }
    }

    ScopedEnv(const ScopedEnv &)            = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;
    ScopedEnv(ScopedEnv &&)                 = delete;
    ScopedEnv &operator=(ScopedEnv &&)      = delete;

private:
    const char                *name_;
    std::optional<std::string> previous_;
};

/* The wrapper reads HIPOBJ_DMABUF when it's created, not when the program
 * starts, so a program can set it before calling hipObjInit() */
TEST_F(FakeDmabufTest, SettingIsReadWhenWrapperIsCreated)
{
    {
        const ScopedEnv off("HIPOBJ_DMABUF", "0");
        fakeIbv_->replace(fakeIbv_->funcs());
        hipObj::IbvMrPtr mr = registerFakeBuffer();
        ASSERT_NE(mr, nullptr);
        EXPECT_EQ(g_log.exportCalls, 0);
    }
    {
        const ScopedEnv on("HIPOBJ_DMABUF", "1");
        fakeIbv_->replace(fakeIbv_->funcs());
        hipObj::IbvMrPtr mr = registerFakeBuffer();
        ASSERT_NE(mr, nullptr);
        EXPECT_EQ(g_log.exportCalls, 1);
    }
}

TEST_F(DmabufDisabledTest, RegistersWithoutExporting)
{
    hipObj::IbvMrPtr mr = registerFakeBuffer();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->addr, fakeDevBuf());
    EXPECT_EQ(g_log.exportCalls, 0);
    EXPECT_EQ(g_log.dmabufRegisterCalls, 0);
    EXPECT_EQ(g_log.plainRegisterCalls, 1);
    EXPECT_EQ(g_log.closeCalls, 0);
}

} // namespace

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
