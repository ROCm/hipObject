/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RDMA topology and NIC selection.
 */

#pragma once

#include <cstddef>
#include <cstdint>

struct ibv_context;

namespace hipObj {

/* Higher value = more preferred. */
enum GidPriority {
    GID_UNKNOWN       = -1,
    ROCEV1_LINK_LOCAL = 0,
    ROCEV1_GLOBAL     = 1,
    ROCEV2_LINK_LOCAL = 2,
    ROCEV2_GLOBAL     = 3,
    ROCEV2_IPV4       = 5,
};

/* One GID table entry as read from the device: its index, raw GID, and
 * RoCE version from sysfs gid_attrs/types (1, 2, or anything else for
 * unknown). */
struct GidCandidate {
    int     index;
    uint8_t raw[16];
    int     roceVersion;
};

/* Pick the preferred GID: unconfigured (all-zero) and unknown-type
 * entries are skipped, the highest GidPriority wins, and ties keep the
 * earliest candidate. Returns that candidate's index, or -1 if none
 * qualifies. */
int PickBestGid(const GidCandidate *candidates, size_t count);

int GetClosestNicToGpu(int gpuIndex, const char *hca_list, const char **dev_name);

int SelectBestGid(ibv_context *ctx, uint8_t port_num);

} // namespace hipObj
