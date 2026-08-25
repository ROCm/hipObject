/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * NIC topology seam.
 *
 * NIC selection walks the ibverbs device list and sysfs to build the
 * candidate set (device name, PCIe bus id, NUMA node, port). Wrapping
 * the enumeration behind an interface lets unit tests inject a fake
 * topology (no RDMA hardware required); production keeps the sysfs
 * walk. The selection algorithm itself (bus-id distance) is pure and
 * shared by both.
 */

#pragma once

#include <string>
#include <vector>

namespace hipObj {

struct NicInfo {
  std::string dev_name;
  std::string pcie_bus_id;
  int numa_node = -1;
  int port_num = 1;
};

/* Enumerates candidate RDMA NICs for the given HCA filter. The default
 * implementation walks ibverbs + sysfs; tests substitute a fake. */
class NicEnumerator {
public:
  virtual ~NicEnumerator() = default;
  virtual std::vector<NicInfo> Enumerate(const char* hca_list) = 0;
};

/* Global enumerator used by GetClosestNicToGpu. Returns the default
 * sysfs walker when no override is installed. */
NicEnumerator& nicEnumerator();

/* Installs an override (nullptr restores the default) and returns the
 * previously installed override (nullptr if the default was active).
 * Tests swap and restore around each case; production never calls it. */
NicEnumerator* setNicEnumerator(NicEnumerator* enumerator);

} // namespace hipObj
