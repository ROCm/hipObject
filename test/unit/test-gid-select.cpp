/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for RoCE GID selection (PickBestGid). Hardware-free: the
 * GID table entries are built in memory.
 */

#include <vector>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "rdma-topology.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObj::GidCandidate;
using hipObj::PickBestGid;

GidCandidate
linkLocal(int index, int roceVersion)
{
    GidCandidate c{};
    c.index       = index;
    c.gid[0]      = 0xfe;
    c.gid[1]      = 0x80;
    c.gid[8]      = 0x6e;
    c.gid[15]     = 0x80;
    c.roceVersion = roceVersion;
    return c;
}

GidCandidate
ipv4Mapped(int index, int roceVersion)
{
    GidCandidate c{};
    c.index       = index;
    c.gid[10]     = 0xff;
    c.gid[11]     = 0xff;
    c.gid[12]     = 100;
    c.gid[13]     = 68;
    c.gid[14]     = 213;
    c.gid[15]     = 79;
    c.roceVersion = roceVersion;
    return c;
}

GidCandidate
globalV6(int index, int roceVersion)
{
    GidCandidate c{};
    c.index       = index;
    c.gid[0]      = 0x20;
    c.gid[1]      = 0x01;
    c.gid[2]      = 0x0d;
    c.gid[3]      = 0xb8;
    c.gid[15]     = 0x01;
    c.roceVersion = roceVersion;
    return c;
}

int
pick(const std::vector<GidCandidate> &v)
{
    return PickBestGid(v.data(), v.size());
}

} // namespace

// GID table of a Broadcom BCM57608 port (bnxt_re): link-local and
// IPv4-mapped GIDs, each as RoCE v1 and RoCE v2. The RoCE v2 IPv4 entry
// must win; the first entry (RoCE v1 link-local) is the least preferred.
TEST(GidSelect, PrefersRoceV2Ipv4OnTypicalRoceTable)
{
    EXPECT_EQ(pick({linkLocal(0, 1), linkLocal(1, 2), ipv4Mapped(2, 1), ipv4Mapped(3, 2)}), 3);
}

TEST(GidSelect, ResultDoesNotDependOnTableOrder)
{
    EXPECT_EQ(pick({ipv4Mapped(3, 2), linkLocal(1, 2), ipv4Mapped(2, 1), linkLocal(0, 1)}), 3);
}

TEST(GidSelect, RoceV2GlobalBeatsRoceV2LinkLocal)
{
    EXPECT_EQ(pick({linkLocal(0, 1), linkLocal(1, 2), globalV6(2, 2)}), 2);
}

TEST(GidSelect, RoceV2LinkLocalBeatsAnyRoceV1)
{
    EXPECT_EQ(pick({linkLocal(0, 1), ipv4Mapped(1, 1), linkLocal(2, 2)}), 2);
}

TEST(GidSelect, RoceV1OnlyPrefersGlobalOverLinkLocal)
{
    EXPECT_EQ(pick({linkLocal(0, 1), ipv4Mapped(1, 1)}), 1);
}

TEST(GidSelect, TieKeepsEarliestCandidate)
{
    EXPECT_EQ(pick({ipv4Mapped(4, 2), ipv4Mapped(5, 2)}), 4);
}

TEST(GidSelect, SkipsUnconfiguredAndUnknownTypeEntries)
{
    GidCandidate zero{};
    zero.index               = 0;
    zero.roceVersion         = 2;
    GidCandidate unknownType = ipv4Mapped(1, -1);
    EXPECT_EQ(pick({zero, unknownType, linkLocal(2, 1)}), 2);
}

TEST(GidSelect, NoUsableEntryReturnsMinusOne)
{
    GidCandidate zero{};
    zero.index       = 0;
    zero.roceVersion = 2;
    EXPECT_EQ(pick({}), -1);
    EXPECT_EQ(pick({zero, ipv4Mapped(1, 0)}), -1);
}

HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
