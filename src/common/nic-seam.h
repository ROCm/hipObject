/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * NIC topology seam.
 *
 * NIC and GID selection need what libibverbs doesn't report: each NIC's
 * PCIe bus id and NUMA node, and the RoCE version of each GID table
 * entry. They come through NicTopology, so unit tests can substitute a
 * fake topology (no RDMA hardware required). SysfsTopology, the
 * production implementation, reads them from sysfs. The selection
 * algorithms themselves (bus-id distance, GID priority) are pure and
 * shared by both.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hipObj {

class Verbs;

struct NicInfo {
    std::string dev_name{};
    std::string pcie_bus_id{};
    int         numa_node = -1;
    int         port_num  = 1;
};

/* The methods are const, so one topology can be shared by everything
 * that uses it. A fake that records its calls keeps what it records
 * outside the object, behind a pointer. */
class NicTopology {
public:
    NicTopology()          = default;
    virtual ~NicTopology() = default;

    /* Copying through a reference to the base would slice */
    NicTopology(const NicTopology &)            = delete;
    NicTopology &operator=(const NicTopology &) = delete;
    NicTopology(NicTopology &&)                 = delete;
    NicTopology &operator=(NicTopology &&)      = delete;

    /* The NICs with an active port, each with its first active port,
     * among those that hca_list (comma-separated device names, or null or
     * empty for all) allows */
    virtual std::vector<NicInfo> enumerate(const char *hca_list) const = 0;

    /* The RoCE version of entry gidIndex in the GID table of port portNum
     * on device devName: 1 or 2, 0 for another type, or -1 when it can't
     * be read */
    virtual int roceVersion(const char *devName, uint8_t portNum, int gidIndex) const = 0;
};

/* The system's topology: the devices from verbs, and the rest from
 * /sys/class/infiniband */
class SysfsTopology final : public NicTopology {
public:
    /* verbs has to outlive it. The owning pointers enumerate() uses
     * release what they hold through ibv(), so verbs also has to be what
     * ibv() returns. */
    explicit SysfsTopology(const Verbs &verbs) noexcept : verbs_(&verbs)
    {
    }
    ~SysfsTopology() override = default;

    SysfsTopology(const SysfsTopology &)            = delete;
    SysfsTopology &operator=(const SysfsTopology &) = delete;
    SysfsTopology(SysfsTopology &&)                 = delete;
    SysfsTopology &operator=(SysfsTopology &&)      = delete;

    std::vector<NicInfo> enumerate(const char *hca_list) const override;
    int                  roceVersion(const char *devName, uint8_t portNum, int gidIndex) const override;

private:
    const Verbs *verbs_;
};

/* Installs a topology for NIC and GID selection to use, and returns the
 * previously installed one (nullptr when none was). Passing nullptr
 * restores the default, a SysfsTopology over ibv(). Unit tests only;
 * production never calls it. Not synchronized, so call it only while no
 * other thread can be selecting a NIC or GID. */
const NicTopology *setNicTopologyForTest(const NicTopology *topology);

} // namespace hipObj
