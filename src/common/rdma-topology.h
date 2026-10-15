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

class HipRuntime;
class NicTopology;
class Verbs;
enum class RoceVersion;

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
 * RoCE version from NicTopology::roceVersion(). */
struct GidCandidate {
    int         index;
    Gid         gid;
    RoceVersion roceVersion;
};

/* Pick the preferred GID: unconfigured (all-zero) and unknown-type
 * entries are skipped, the highest GidPriority wins, and ties keep the
 * earliest candidate. Returns that candidate's index, or std::nullopt if
 * none qualifies. */
std::optional<int> PickBestGid(std::span<const GidCandidate> candidates);

/* An RDMA NIC: its index in the list NicTopology::enumerate() returned,
 * and its device name */
struct RdmaNic {
    int         index;
    std::string name;
};

/* The NIC in topology with a known PCIe bus id closest to the bus id
 * runtime reports for GPU gpuIndex, picking the first of the closest,
 * among the NICs that hca_list (comma-separated device names, or null for
 * all) allows. std::nullopt when there's no such NIC or the GPU's bus id
 * can't be found. */
std::optional<RdmaNic> GetClosestNicToGpu(const HipRuntime &runtime, const NicTopology &topology,
                                          int gpuIndex, const char *hca_list);

/* PickBestGid() over the GID table that verbs reports for port_num on
 * ctx's device, with each entry's RoCE version from topology.
 * std::nullopt when the port can't be queried or no entry qualifies. */
std::optional<int> SelectBestGid(const Verbs &verbs, const NicTopology &topology, ibv_context *ctx,
                                 uint8_t port_num);

} // namespace hipObj
