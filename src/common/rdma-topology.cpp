/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RDMA topology implementation.
 */

#include "rdma-topology.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "ibv-core.h"
#include "ibv-gid.h"
#include "ibv-ptr.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"

namespace hipObj {

namespace {

    bool IsConfiguredGid(const Gid &gid)
    {
        return gid != Gid{};
    }

    bool LinkLocalGid(const Gid &gid)
    {
        return gid[0] == 0xfe && gid[1] == 0x80;
    }

    bool IsIPv4MappedIPv6(const Gid &gid)
    {
        return gid[0] == 0 && gid[1] == 0 && gid[2] == 0 && gid[3] == 0 && gid[4] == 0 && gid[5] == 0 &&
               gid[6] == 0 && gid[7] == 0 && gid[8] == 0 && gid[9] == 0 && gid[10] == 0xff && gid[11] == 0xff;
    }

    GidPriority ClassifyGid(const Gid &gid, RoceVersion roce_ver)
    {
        switch (roce_ver) {
            case RoceVersion::V2:
                if (IsIPv4MappedIPv6(gid))
                    return GidPriority::RoceV2Ipv4;
                if (LinkLocalGid(gid))
                    return GidPriority::RoceV2LinkLocal;
                return GidPriority::RoceV2Global;
            case RoceVersion::V1:
                return LinkLocalGid(gid) ? GidPriority::RoceV1LinkLocal : GidPriority::RoceV1Global;
            case RoceVersion::Unknown:
            default:
                return GidPriority::Unknown;
        }
    }

    std::optional<int> AutoSelectGidIndex(const Verbs &verbs, const NicTopology &topology, ibv_context *ctx,
                                          uint8_t port_num)
    {
        ibv_port_attr port_attr;
        if (verbs.query_port(ctx, port_num, &port_attr) != 0)
            return std::nullopt;

        std::vector<GidCandidate> candidates;
        for (int i = 0; i < port_attr.gid_tbl_len; ++i) {
            ibv_gid gid;
            if (verbs.query_gid(ctx, port_num, i, &gid) != 0)
                continue;
            GidCandidate c;
            c.index = i;
            c.gid   = toGid(gid);
            if (!IsConfiguredGid(c.gid))
                continue;
            c.roceVersion = topology.roceVersion(ctx->device->name, port_num, i);
            candidates.push_back(c);
        }
        return PickBestGid(candidates);
    }

    std::string ExtractBusNumber(const std::string &pcie_bus_id)
    {
        size_t last_colon = pcie_bus_id.rfind(':');
        if (last_colon == std::string::npos)
            return pcie_bus_id;
        return pcie_bus_id.substr(0, last_colon);
    }

    int GetBusIdDistance(const std::string &bus_a, const std::string &bus_b)
    {
        if (bus_a.empty() || bus_b.empty())
            return std::numeric_limits<int>::max();
        std::string a = ExtractBusNumber(bus_a);
        std::string b = ExtractBusNumber(bus_b);
        if (a == b)
            return 0;
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i])
            ++i;
        return static_cast<int>(a.size() - i) + static_cast<int>(b.size() - i);
    }

} // namespace

std::vector<NicInfo>
SysfsTopology::enumerate(const char *hca_list) const
{
    std::vector<NicInfo> result;
    int                  num_devs = 0;
    IbvDeviceListPtr     dev_list = own<IbvDeviceListPtr>(*verbs_, verbs_->get_device_list(&num_devs));
    if (!dev_list || num_devs <= 0)
        return result;

    for (ibv_device *dev : deviceSpan(dev_list, num_devs)) {
        if (!dev)
            break;
        const char *dev_name = verbs_->get_device_name(dev);
        if (!dev_name)
            continue;
        if (hca_list && *hca_list) {
            bool               match = false;
            std::istringstream iss(hca_list);
            std::string        hca;
            while (std::getline(iss, hca, ',')) {
                size_t pos = hca.find_first_not_of(" \t");
                if (pos != std::string::npos)
                    hca = hca.substr(pos);
                if (hca == dev_name) {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
        }

        IbvContextPtr ctx = own<IbvContextPtr>(*verbs_, verbs_->open_device(dev));
        if (!ctx)
            continue;

        ibv_device_attr attr;
        if (verbs_->query_device(ctx.get(), &attr) != 0) {
            continue;
        }

        for (uint8_t p = 1; p <= attr.phys_port_cnt; ++p) {
            ibv_port_attr port_attr;
            if (verbs_->query_port(ctx.get(), p, &port_attr) != 0)
                continue;
            if (port_attr.state != IBV_PORT_ACTIVE && port_attr.state != IBV_PORT_ACTIVE_DEFER)
                continue;

            NicInfo info;
            info.dev_name = dev_name;
            info.port_num = static_cast<int>(p);

            const std::filesystem::path device_path =
                std::filesystem::path("/sys/class/infiniband") / dev_name / "device";
            /* The device link resolves to the PCI device's directory,
             * which is named after its bus id */
            std::error_code             ec;
            const std::filesystem::path resolved = std::filesystem::canonical(device_path, ec);
            if (!ec)
                info.pcie_bus_id = resolved.filename().string();

            std::ifstream numa_file(device_path / "numa_node");
            if (numa_file) {
                numa_file >> info.numa_node;
                if (info.numa_node < 0)
                    info.numa_node = 0;
            }

            result.push_back(info);
            break;
        }
    }
    return result;
}

RoceVersion
SysfsTopology::roceVersion(const char *devName, uint8_t portNum, int gidIndex) const
{
    const std::filesystem::path path = std::filesystem::path("/sys/class/infiniband") / devName / "ports" /
                                       std::to_string(static_cast<unsigned int>(portNum)) /
                                       "gid_attrs/types" / std::to_string(gidIndex);
    std::ifstream f(path);
    if (!f)
        return RoceVersion::Unknown;
    std::string line;
    if (!std::getline(f, line))
        return RoceVersion::Unknown;
    if (line.find("RoCE v2") != std::string::npos)
        return RoceVersion::V2;
    if (line.find("IB/RoCE v1") != std::string::npos || line.find("RoCE v1") != std::string::npos)
        return RoceVersion::V1;
    return RoceVersion::Unknown;
}

std::optional<RdmaNic>
GetClosestNicToGpu(const HipRuntime &runtime, const NicTopology &topology, int gpuIndex, const char *hca_list)
{
    char       gpu_bus_id[32];
    hipError_t err = runtime.deviceGetPCIBusId(gpu_bus_id, sizeof(gpu_bus_id), gpuIndex);
    if (err != hipSuccess)
        return std::nullopt;

    std::string gpu_bus(gpu_bus_id);
    auto        devices = topology.enumerate(hca_list);
    if (devices.empty())
        return std::nullopt;

    auto distance = [&gpu_bus](const NicInfo &nic) { return GetBusIdDistance(nic.pcie_bus_id, gpu_bus); };
    /* The first of the closest NICs */
    auto best = std::ranges::min_element(devices, {}, distance);
    /* No NIC has a known bus id */
    if (distance(*best) == std::numeric_limits<int>::max())
        return std::nullopt;

    return RdmaNic{static_cast<int>(best - devices.begin()), std::move(best->dev_name)};
}

std::optional<int>
PickBestGid(std::span<const GidCandidate> candidates)
{
    std::optional<int> best_idx;
    GidPriority        best_prio = GidPriority::Unknown;
    for (const GidCandidate &c : candidates) {
        if (!IsConfiguredGid(c.gid))
            continue;
        GidPriority prio = ClassifyGid(c.gid, c.roceVersion);
        if (prio != GidPriority::Unknown && (!best_idx || prio > best_prio)) {
            best_idx  = c.index;
            best_prio = prio;
        }
    }
    return best_idx;
}

std::optional<int>
SelectBestGid(const Verbs &verbs, const NicTopology &topology, ibv_context *ctx, uint8_t port_num)
{
    return AutoSelectGidIndex(verbs, topology, ctx, port_num);
}

} // namespace hipObj
