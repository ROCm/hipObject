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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "hip-seam.h"
#include "hipobj-span.h"
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

    /* The first two bytes of a link-local IPv6 address */
    constexpr auto kLinkLocalPrefix = std::to_array<uint8_t>({0xfe, 0x80});

    bool LinkLocalGid(const Gid &gid)
    {
        return std::ranges::equal(std::span(gid).first<kLinkLocalPrefix.size()>(), kLinkLocalPrefix);
    }

    GidPriority ClassifyGid(const Gid &gid, RoceVersion roce_ver)
    {
        switch (roce_ver) {
            case RoceVersion::V2:
                if (isIpv4MappedGid(gid)) {
                    return GidPriority::RoceV2Ipv4;
                }
                if (LinkLocalGid(gid)) {
                    return GidPriority::RoceV2LinkLocal;
                }
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
        ibv_port_attr port_attr{};
        if (verbs.query_port(ctx, port_num, &port_attr) != 0) {
            return std::nullopt;
        }

        std::vector<GidCandidate> candidates;
        for (int i = 0; i < port_attr.gid_tbl_len; ++i) {
            ibv_gid gid{};
            if (verbs.query_gid(ctx, port_num, i, &gid) != 0) {
                continue;
            }
            GidCandidate c{};
            c.index = i;
            c.gid   = toGid(gid);
            if (!IsConfiguredGid(c.gid)) {
                continue;
            }
            c.roceVersion = topology.roceVersion(cStringView(ctx->device->name), port_num, i);
            candidates.push_back(c);
        }
        return PickBestGid(candidates);
    }

    std::string ExtractBusNumber(const std::string &pcie_bus_id)
    {
        const size_t last_colon = pcie_bus_id.rfind(':');
        if (last_colon == std::string::npos) {
            return pcie_bus_id;
        }
        return pcie_bus_id.substr(0, last_colon);
    }

    int GetBusIdDistance(const std::string &bus_a, const std::string &bus_b)
    {
        if (bus_a.empty() || bus_b.empty()) {
            return std::numeric_limits<int>::max();
        }
        std::string a = ExtractBusNumber(bus_a);
        std::string b = ExtractBusNumber(bus_b);
        if (a == b) {
            return 0;
        }
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i]) {
            ++i;
        }
        return static_cast<int>(a.size() - i) + static_cast<int>(b.size() - i);
    }

    /* Whether hcaList, a list of device names separated by commas, each of
     * which can have spaces and tabs before it, includes devName */
    bool HcaListIncludes(std::string_view hcaList, std::string_view devName)
    {
        for (;;) {
            const size_t     comma = hcaList.find(',');
            std::string_view hca   = hcaList.substr(0, comma);
            const size_t     start = hca.find_first_not_of(" \t");
            if (start != std::string_view::npos) {
                hca.remove_prefix(start);
            }
            if (hca == devName) {
                return true;
            }
            if (comma == std::string_view::npos) {
                return false;
            }
            hcaList.remove_prefix(comma + 1);
        }
    }

} // namespace

std::vector<NicInfo>
SysfsTopology::enumerate(std::string_view hcaList) const
{
    std::vector<NicInfo> result;
    int                  num_devs = 0;
    const auto           dev_list = own<IbvDeviceListPtr>(*verbs_, verbs_->get_device_list(&num_devs));
    if (!dev_list || num_devs <= 0) {
        return result;
    }

    for (ibv_device *dev : deviceSpan(dev_list, num_devs)) {
        if (dev == nullptr) {
            break;
        }
        const char *dev_name = verbs_->get_device_name(dev);
        if (dev_name == nullptr) {
            continue;
        }
        if (!hcaList.empty() && !HcaListIncludes(hcaList, dev_name)) {
            continue;
        }

        const auto ctx = own<IbvContextPtr>(*verbs_, verbs_->open_device(dev));
        if (!ctx) {
            continue;
        }

        ibv_device_attr attr{};
        if (verbs_->query_device(ctx.get(), &attr) != 0) {
            continue;
        }

        for (uint8_t p = 1; p <= attr.phys_port_cnt; ++p) {
            ibv_port_attr port_attr{};
            if (verbs_->query_port(ctx.get(), p, &port_attr) != 0) {
                continue;
            }
            if (port_attr.state != IBV_PORT_ACTIVE && port_attr.state != IBV_PORT_ACTIVE_DEFER) {
                continue;
            }

            NicInfo info;
            info.dev_name = dev_name;
            info.port_num = static_cast<int>(p);

            const std::filesystem::path device_path =
                std::filesystem::path("/sys/class/infiniband") / dev_name / "device";
            /* The device link resolves to the PCI device's directory,
             * which is named after its bus id */
            std::error_code             ec;
            const std::filesystem::path resolved = std::filesystem::canonical(device_path, ec);
            if (!ec) {
                info.pcie_bus_id = resolved.filename().string();
            }

            std::ifstream numa_file(device_path / "numa_node");
            if (numa_file) {
                numa_file >> info.numa_node;
                info.numa_node = std::max(info.numa_node, 0);
            }

            result.push_back(info);
            break;
        }
    }
    return result;
}

RoceVersion
SysfsTopology::roceVersion(std::string_view devName, uint8_t portNum, int gidIndex) const
{
    const std::filesystem::path path = std::filesystem::path("/sys/class/infiniband") / devName / "ports" /
                                       std::to_string(static_cast<unsigned int>(portNum)) /
                                       "gid_attrs/types" / std::to_string(gidIndex);
    std::ifstream f(path);
    if (!f) {
        return RoceVersion::Unknown;
    }
    std::string line;
    if (!std::getline(f, line)) {
        return RoceVersion::Unknown;
    }
    if (line.find("RoCE v2") != std::string::npos) {
        return RoceVersion::V2;
    }
    if (line.find("IB/RoCE v1") != std::string::npos || line.find("RoCE v1") != std::string::npos) {
        return RoceVersion::V1;
    }
    return RoceVersion::Unknown;
}

std::optional<RdmaNic>
GetClosestNicToGpu(const HipRuntime &runtime, const NicTopology &topology, int gpuIndex,
                   std::string_view hcaList)
{
    std::array<char, kPciBusIdBufLen> gpu_bus_id{};
    const int                         len = static_cast<int>(gpu_bus_id.size());
    if (runtime.deviceGetPCIBusId(gpu_bus_id.data(), len, gpuIndex) != hipSuccess) {
        return std::nullopt;
    }

    /* Up to the first NUL, or the whole array if HIP didn't write one */
    std::string gpu_bus(gpu_bus_id.begin(), std::ranges::find(gpu_bus_id, '\0'));
    auto        devices = topology.enumerate(hcaList);
    if (devices.empty()) {
        return std::nullopt;
    }

    const auto distance = [&gpu_bus](const NicInfo &nic) {
        return GetBusIdDistance(nic.pcie_bus_id, gpu_bus);
    };
    /* The first of the closest NICs */
    const auto best = std::ranges::min_element(devices, {}, distance);
    /* No NIC has a known bus id */
    if (distance(*best) == std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    return RdmaNic{.index = static_cast<int>(best - devices.begin()), .name = std::move(best->dev_name)};
}

std::optional<int>
PickBestGid(std::span<const GidCandidate> candidates)
{
    std::optional<int> best_idx;
    GidPriority        best_prio = GidPriority::Unknown;
    for (const GidCandidate &c : candidates) {
        if (!IsConfiguredGid(c.gid)) {
            continue;
        }
        const GidPriority prio = ClassifyGid(c.gid, c.roceVersion);
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
