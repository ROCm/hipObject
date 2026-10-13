/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RDMA topology and NIC selection.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "gid.h"

struct ibv_context;

namespace hipObj {

/* Higher value = more preferred. */
enum class GidPriority {
    Unknown         = -1,
    RoceV1LinkLocal = 0,
    RoceV1Global    = 1,
    RoceV2LinkLocal = 2,
    RoceV2Global    = 3,
    RoceV2Ipv4      = 5,
};

/* One GID table entry as read from the device: its index, GID, and
 * RoCE version from sysfs gid_attrs/types (1, 2, or anything else for
 * unknown). */
struct GidCandidate {
    int index;
    Gid gid;
    int roceVersion;
};

/* Pick the preferred GID: unconfigured (all-zero) and unknown-type
 * entries are skipped, the highest GidPriority wins, and ties keep the
 * earliest candidate. Returns that candidate's index, or -1 if none
 * qualifies. */
int PickBestGid(std::span<const GidCandidate> candidates);

/* An RDMA NIC: its index in the NIC enumerator's list, and its device
 * name */
struct RdmaNic {
    int         index;
    std::string name;
};

/* The NIC with a known PCIe bus id closest to the GPU's, picking the
 * first of the closest, among the NICs that hca_list (comma-separated
 * device names, or null for all) allows. std::nullopt when there's no
 * such NIC or the GPU's bus id can't be found. */
std::optional<RdmaNic> GetClosestNicToGpu(int gpuIndex, const char *hca_list);

int SelectBestGid(ibv_context *ctx, uint8_t port_num);

} // namespace hipObj
