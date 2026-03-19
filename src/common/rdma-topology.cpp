/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * RDMA topology implementation.
 */

#include "rdma-topology.hpp"

#include <limits.h>
#include <stdlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <hip/hip_runtime.h>

#include <unistd.h>

#include "ibv-wrapper.hpp"
#include "rdma-numa-wrapper.hpp"

namespace hipObj {

namespace {

struct IbvDeviceInfo {
  std::string dev_name;
  std::string pcie_bus_id;
  int numa_node = -1;
  int port_num = 1;
};

bool IsConfiguredGid(const uint8_t gid[16]) {
  for (int i = 0; i < 16; ++i) {
    if (gid[i] != 0)
      return true;
  }
  return false;
}

bool LinkLocalGid(const uint8_t gid[16]) {
  return gid[0] == 0xfe && gid[1] == 0x80;
}

bool IsIPv4MappedIPv6(const uint8_t gid[16]) {
  return gid[0] == 0 && gid[1] == 0 && gid[2] == 0 && gid[3] == 0 &&
         gid[4] == 0 && gid[5] == 0 && gid[6] == 0 && gid[7] == 0 &&
         gid[8] == 0 && gid[9] == 0 && gid[10] == 0xff && gid[11] == 0xff;
}

int GetRoceVersionNumber(const char* dev_name, int port_num, int gid_idx) {
  std::ostringstream path;
  path << "/sys/class/infiniband/" << dev_name << "/ports/" << port_num
       << "/gid_attrs/types/" << gid_idx;
  std::ifstream f(path.str());
  if (!f)
    return -1;
  std::string line;
  if (!std::getline(f, line))
    return -1;
  if (line.find("RoCE v2") != std::string::npos)
    return 2;
  if (line.find("IB/RoCE v1") != std::string::npos ||
      line.find("RoCE v1") != std::string::npos)
    return 1;
  return 0;
}

int AutoSelectGidIndex(ibv_context* ctx, uint8_t port_num) {
  ibv_port_attr port_attr;
  if (ibv.query_port(ctx, port_num, &port_attr) != 0)
    return -1;

  int best_idx = -1;
  GidPriority best_prio = GID_UNKNOWN;

  for (int i = 0; i < port_attr.gid_tbl_len; ++i) {
    ibv_gid gid;
    if (ibv.query_gid(ctx, port_num, i, &gid) != 0)
      continue;
    if (!IsConfiguredGid(gid.raw))
      continue;

    int roce_ver = GetRoceVersionNumber(ctx->device->name, port_num, i);
    GidPriority prio = GID_UNKNOWN;
    if (roce_ver == 2) {
      if (IsIPv4MappedIPv6(gid.raw))
        prio = ROCEV2_IPV4;
      else if (LinkLocalGid(gid.raw))
        prio = ROCEV2_LINK_LOCAL;
      else
        prio = ROCEV2_GLOBAL;
    } else if (roce_ver == 1) {
      if (LinkLocalGid(gid.raw))
        prio = ROCEV1_LINK_LOCAL;
      else
        prio = ROCEV1_GLOBAL;
    }

    if (prio != GID_UNKNOWN && (best_idx < 0 || prio < best_prio)) {
      best_idx = i;
      best_prio = prio;
    }
  }
  return best_idx;
}

std::string ExtractBusNumber(const std::string& pcie_bus_id) {
  size_t last_colon = pcie_bus_id.rfind(':');
  if (last_colon == std::string::npos)
    return pcie_bus_id;
  return pcie_bus_id.substr(0, last_colon);
}

int GetBusIdDistance(const std::string& bus_a, const std::string& bus_b) {
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

std::vector<IbvDeviceInfo> GetIbvDeviceList(const char* hca_list) {
  std::vector<IbvDeviceInfo> result;
  int num_devs = 0;
  ibv_device** dev_list = ibv.get_device_list(&num_devs);
  if (!dev_list || num_devs <= 0)
    return result;

  for (int i = 0; i < num_devs && dev_list[i]; ++i) {
    ibv_device* dev = dev_list[i];
    const char* dev_name = ibv.get_device_name(dev);
    if (!dev_name)
      continue;
    if (hca_list && *hca_list) {
      bool match = false;
      std::istringstream iss(hca_list);
      std::string hca;
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

    ibv_context* ctx = ibv.open_device(dev);
    if (!ctx)
      continue;

    ibv_device_attr attr;
    if (ibv.query_device(ctx, &attr) != 0) {
      ibv.close_device(ctx);
      continue;
    }

    for (uint8_t p = 1; p <= attr.phys_port_cnt; ++p) {
      ibv_port_attr port_attr;
      if (ibv.query_port(ctx, p, &port_attr) != 0)
        continue;
      if (port_attr.state != IBV_PORT_ACTIVE &&
          port_attr.state != IBV_PORT_ACTIVE_DEFER)
        continue;

      IbvDeviceInfo info;
      info.dev_name = dev_name;
      info.port_num = static_cast<int>(p);

      std::string device_path = "/sys/class/infiniband/";
      device_path += dev_name;
      device_path += "/device";
      char resolved[PATH_MAX];
      if (realpath(device_path.c_str(), resolved)) {
        std::string r(resolved);
        size_t slash = r.rfind('/');
        if (slash != std::string::npos)
          info.pcie_bus_id = r.substr(slash + 1);
      }

      std::string numa_path = device_path;
      numa_path += "/numa_node";
      std::ifstream numa_file(numa_path);
      if (numa_file) {
        numa_file >> info.numa_node;
        if (info.numa_node < 0)
          info.numa_node = 0;
      }

      result.push_back(info);
      break;
    }
    ibv.close_device(ctx);
  }
  ibv.free_device_list(dev_list);
  return result;
}

} // namespace

int GetClosestNicToGpu(int gpuIndex, const char* hca_list,
                       const char** dev_name) {
  char gpu_bus_id[32];
  hipError_t err = hipDeviceGetPCIBusId(gpu_bus_id, sizeof(gpu_bus_id),
                                        gpuIndex);
  if (err != hipSuccess)
    return -1;

  std::string gpu_bus(gpu_bus_id);
  auto devices = GetIbvDeviceList(hca_list);
  if (devices.empty())
    return -1;

  int best_idx = -1;
  int best_dist = std::numeric_limits<int>::max();

  for (size_t i = 0; i < devices.size(); ++i) {
    int dist = GetBusIdDistance(devices[i].pcie_bus_id, gpu_bus);
    if (dist < best_dist) {
      best_dist = dist;
      best_idx = static_cast<int>(i);
    }
  }

  if (best_idx >= 0 && dev_name) {
    static std::string s_dev_name;
    s_dev_name = devices[static_cast<size_t>(best_idx)].dev_name;
    *dev_name = s_dev_name.c_str();
  }
  return best_idx;
}

int SelectBestGid(ibv_context* ctx, uint8_t port_num) {
  return AutoSelectGidIndex(ctx, port_num);
}

} // namespace hipObj
