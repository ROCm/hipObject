/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sending the RDMA token through the caller's S3 callbacks and reading the
 * reply. The public API checks the callbacks before it gets here, so these
 * tests call the functions directly.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "control.h"
#include "hipobj-span.h"
#include "hipobj-warnings.h"
#include "hipobj.h"
#include "token.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

/* What the fake callbacks saw, and what they return. A nonzero
 * extraLen makes recvReply report that many more bytes than it wrote. */
struct Callbacks {
    size_t           tokenLen = 0;
    int              sendRet  = 0;
    int              recvRet  = 0;
    std::string_view reply    = "200";
    size_t           extraLen = 0;
    void            *ctx      = nullptr;
};

Callbacks g_callbacks;

int
fakeSendRequest(void *ctx, const char * /*token*/, size_t tokenLen)
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
    std::ranges::copy(g_callbacks.reply,
                      hipObj::spanFromPointer(reply, *replyLen).first(g_callbacks.reply.size()).begin());
    *replyLen = g_callbacks.reply.size() + g_callbacks.extraLen;
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

    std::optional<hipObj::RdmaReply> receive()
    {
        return hipObj::receiveRdmaReplyRaw(ops_, &ctx_, buf_);
    }

    hipObjOps_t           ops_{};
    int                   ctx_ = 0;
    std::array<char, 512> buf_{};
};

TEST_F(S3ControlTest, SendsTokenToCallback)
{
    EXPECT_TRUE(hipObj::injectRdmaToken(ops_, &ctx_, "abcd"));
    EXPECT_EQ(g_callbacks.tokenLen, 4U);
    EXPECT_EQ(g_callbacks.ctx, &ctx_);
}

TEST_F(S3ControlTest, SendReportsCallbackFailure)
{
    g_callbacks.sendRet = 7;
    EXPECT_FALSE(hipObj::injectRdmaToken(ops_, &ctx_, "abcd"));
}

TEST_F(S3ControlTest, SendNeedsCallback)
{
    ops_.sendRequest = nullptr;
    EXPECT_FALSE(hipObj::injectRdmaToken(ops_, &ctx_, "abcd"));
}

TEST_F(S3ControlTest, ReceiveDecodesStatus)
{
    auto reply = receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, hipObj::RdmaReplyStatus::Success);
    EXPECT_EQ(reply->text, "200");
    EXPECT_EQ(reply->text.data(), buf_.data());
    EXPECT_EQ(g_callbacks.ctx, &ctx_);

    g_callbacks.reply = "404";
    reply             = receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, hipObj::RdmaReplyStatus::Failed);

    /* The server doesn't support RDMA */
    g_callbacks.reply = "501";
    reply             = receive();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, hipObj::RdmaReplyStatus::NotSupported);
}

TEST_F(S3ControlTest, ReceiveRejectsMalformedReply)
{
    for (const std::string_view reply : {"", "2000", "abc", "200x"}) {
        g_callbacks.reply = reply;
        EXPECT_FALSE(receive()) << "reply \"" << reply << "\"";
    }
}

/* A length the buffer can't hold must not be read */
TEST_F(S3ControlTest, ReceiveRejectsLengthPastBuffer)
{
    /* The callback writes "200" into a three-byte buffer and reports four
     * bytes. The fourth is the NUL after the buffer, which the parser
     * would skip and accept the reply if it read it. */
    const std::span<char> replyBuf = std::span(buf_).first(3);
    g_callbacks.extraLen           = 1;
    EXPECT_FALSE(hipObj::receiveRdmaReplyRaw(ops_, &ctx_, replyBuf));

    /* Filling the buffer exactly is fine */
    g_callbacks.extraLen = 0;
    const auto reply     = hipObj::receiveRdmaReplyRaw(ops_, &ctx_, replyBuf);
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, hipObj::RdmaReplyStatus::Success);
    EXPECT_EQ(reply->text, "200");
}

TEST_F(S3ControlTest, ReceiveReportsCallbackFailure)
{
    g_callbacks.recvRet = 3;
    EXPECT_FALSE(receive());
}

TEST_F(S3ControlTest, ReceiveNeedsCallbackAndBuffer)
{
    EXPECT_FALSE(hipObj::receiveRdmaReplyRaw(ops_, &ctx_, {}));
    ops_.recvReply = nullptr;
    EXPECT_FALSE(hipObj::receiveRdmaReplyRaw(ops_, &ctx_, buf_));
}

/* receiveRdmaReplyV3() decodes the callback's x-amz-rc-rdma-reply */
TEST_F(S3ControlTest, ReceiveV3DecodesReply)
{
    g_callbacks.reply = "206:0000000000001000";
    const auto reply  = hipObj::receiveRdmaReplyV3(ops_, &ctx_);
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->httpStatus, 206);
    EXPECT_EQ(reply->bytesTransferred, uint64_t{0x1000});
    EXPECT_EQ(g_callbacks.ctx, &ctx_);
}

/* It rejects a reply it can't decode, a length past its buffer, a failed
 * callback, and a missing one */
TEST_F(S3ControlTest, ReceiveV3RejectsWhatItCantUse)
{
    g_callbacks.reply = "200";
    EXPECT_FALSE(hipObj::receiveRdmaReplyV3(ops_, &ctx_));

    g_callbacks.reply    = "200:0000000000000000";
    g_callbacks.extraLen = buf_.size();
    EXPECT_FALSE(hipObj::receiveRdmaReplyV3(ops_, &ctx_));

    g_callbacks.extraLen = 0;
    g_callbacks.recvRet  = 3;
    EXPECT_FALSE(hipObj::receiveRdmaReplyV3(ops_, &ctx_));

    ops_.recvReply = nullptr;
    EXPECT_FALSE(hipObj::receiveRdmaReplyV3(ops_, &ctx_));
}

} // namespace
