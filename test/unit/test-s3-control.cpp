/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sending the RDMA token through the caller's S3 callbacks and reading the
 * reply. The public API checks the callbacks before it gets here, so these
 * tests call the functions directly.
 */

#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "control.h"
#include "hipobj-warnings.h"
#include "hipobj.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* What the fake callbacks saw, and what they return */
struct Callbacks {
    size_t           tokenLen = 0;
    int              sendRet  = 0;
    int              recvRet  = 0;
    std::string_view reply    = "200";
    void            *ctx      = nullptr;
};

Callbacks g_callbacks;

int
fakeSendRequest(void *ctx, const char *, size_t tokenLen)
{
    g_callbacks.ctx      = ctx;
    g_callbacks.tokenLen = tokenLen;
    return g_callbacks.sendRet;
}

int
fakeRecvReply(void *ctx, char *reply, size_t *replyLen)
{
    g_callbacks.ctx = ctx;
    if (g_callbacks.recvRet != 0) {
        return g_callbacks.recvRet;
    }
    std::memcpy(reply, g_callbacks.reply.data(), g_callbacks.reply.size());
    *replyLen = g_callbacks.reply.size();
    return 0;
}

class S3ControlTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        g_callbacks      = {};
        ops_.sendRequest = &fakeSendRequest;
        ops_.recvReply   = &fakeRecvReply;
    }

    int receive(int &status)
    {
        size_t len = buf_.size();
        return hipObj::receiveRdmaReplyRaw(&ops_, &ctx_, buf_.data(), &len, status);
    }

    hipObjOps_t           ops_ = {};
    int                   ctx_ = 0;
    std::array<char, 512> buf_ = {};
};

TEST_F(S3ControlTest, SendsTokenToCallback)
{
    EXPECT_EQ(hipObj::injectRdmaToken(&ops_, &ctx_, "abcd"), 0);
    EXPECT_EQ(g_callbacks.tokenLen, 4U);
    EXPECT_EQ(g_callbacks.ctx, &ctx_);
}

TEST_F(S3ControlTest, SendReportsCallbackFailure)
{
    g_callbacks.sendRet = 7;
    EXPECT_EQ(hipObj::injectRdmaToken(&ops_, &ctx_, "abcd"), 7);
}

TEST_F(S3ControlTest, SendNeedsCallback)
{
    EXPECT_EQ(hipObj::injectRdmaToken(nullptr, &ctx_, "abcd"), -1);
    ops_.sendRequest = nullptr;
    EXPECT_EQ(hipObj::injectRdmaToken(&ops_, &ctx_, "abcd"), -1);
}

TEST_F(S3ControlTest, ReceiveDecodesStatus)
{
    int status = 99;
    ASSERT_EQ(receive(status), 0);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(g_callbacks.ctx, &ctx_);

    g_callbacks.reply = "404";
    ASSERT_EQ(receive(status), 0);
    EXPECT_EQ(status, -1);

    /* The server doesn't support RDMA */
    g_callbacks.reply = "501";
    ASSERT_EQ(receive(status), 0);
    EXPECT_EQ(status, -2);
}

TEST_F(S3ControlTest, ReceiveRejectsMalformedReply)
{
    int status = 0;
    for (std::string_view reply : {"", "2000", "abc", "200x"}) {
        g_callbacks.reply = reply;
        EXPECT_EQ(receive(status), -1) << "reply \"" << reply << "\"";
    }
}

TEST_F(S3ControlTest, ReceiveReportsCallbackFailure)
{
    g_callbacks.recvRet = 3;
    int status          = 0;
    EXPECT_EQ(receive(status), 3);
}

TEST_F(S3ControlTest, ReceiveNeedsCallbackAndBuffer)
{
    int    status = 0;
    size_t len    = buf_.size();
    EXPECT_EQ(hipObj::receiveRdmaReplyRaw(nullptr, &ctx_, buf_.data(), &len, status), -1);
    EXPECT_EQ(hipObj::receiveRdmaReplyRaw(&ops_, &ctx_, nullptr, &len, status), -1);
    EXPECT_EQ(hipObj::receiveRdmaReplyRaw(&ops_, &ctx_, buf_.data(), nullptr, status), -1);
    ops_.recvReply = nullptr;
    EXPECT_EQ(hipObj::receiveRdmaReplyRaw(&ops_, &ctx_, buf_.data(), &len, status), -1);
}

} // namespace
