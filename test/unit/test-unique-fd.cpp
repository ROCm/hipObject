/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for UniqueFd: it closes the descriptor it owns exactly
 * once, and ownership moves with it.
 */

#include <array>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "hipobj-warnings.h"
#include "unique-fd.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::UniqueFd;

bool
isOpen(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

bool
isClosed(int fd)
{
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

/* Returns the two ends of a new pipe, each owned by a UniqueFd */
std::pair<UniqueFd, UniqueFd>
makePipe()
{
    std::array<int, 2> fds{-1, -1};
    if (pipe(fds.data()) != 0) {
        return {};
    }
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

} // namespace

TEST(UniqueFd, DefaultOwnsNothing)
{
    const UniqueFd fd;
    EXPECT_FALSE(fd);
    EXPECT_EQ(-1, fd.get());
}

TEST(UniqueFd, NegativeOwnsNothing)
{
    const UniqueFd fd(-1);
    EXPECT_FALSE(fd);
}

TEST(UniqueFd, ClosesOnDestruction)
{
    auto [r, w] = makePipe();
    ASSERT_TRUE(r);
    ASSERT_TRUE(w);
    const int raw = r.get();
    {
        const UniqueFd owner(std::move(r));
        EXPECT_TRUE(owner);
        EXPECT_EQ(raw, owner.get());
        EXPECT_TRUE(isOpen(raw));
    }
    EXPECT_TRUE(isClosed(raw));
}

TEST(UniqueFd, MoveConstructTransfersOwnership)
{
    auto [r, w] = makePipe();
    ASSERT_TRUE(r);
    const int raw = r.get();

    UniqueFd moved(std::move(r));
    EXPECT_EQ(raw, moved.get());
    /* The moved-from handle owns nothing, so destroying it closes nothing */
    r.reset();
    EXPECT_TRUE(isOpen(raw));
}

TEST(UniqueFd, MoveAssignClosesPrevious)
{
    auto [r, w] = makePipe();
    ASSERT_TRUE(r);
    ASSERT_TRUE(w);
    const int rawR = r.get();
    const int rawW = w.get();

    r = std::move(w);
    EXPECT_TRUE(isClosed(rawR));
    EXPECT_EQ(rawW, r.get());
    EXPECT_FALSE(w);
    EXPECT_TRUE(isOpen(rawW));
}

TEST(UniqueFd, ReleaseGivesUpOwnership)
{
    auto [r, w] = makePipe();
    ASSERT_TRUE(r);
    const int raw = r.get();

    const int released = r.release();
    EXPECT_EQ(raw, released);
    EXPECT_FALSE(r);
    r.reset();
    EXPECT_TRUE(isOpen(released));

    /* Hand it back so the test doesn't leak it */
    r.reset(released);
    EXPECT_EQ(released, r.get());
}

TEST(UniqueFd, ResetClosesAndAdopts)
{
    auto [r, w] = makePipe();
    ASSERT_TRUE(r);
    ASSERT_TRUE(w);
    const int rawR = r.get();
    const int rawW = w.release();

    r.reset(rawW);
    EXPECT_TRUE(isClosed(rawR));
    EXPECT_EQ(rawW, r.get());

    r.reset();
    EXPECT_FALSE(r);
    EXPECT_TRUE(isClosed(rawW));
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
