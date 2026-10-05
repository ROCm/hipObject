# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

add_subdirectory(unit)
add_subdirectory(exports)
add_subdirectory(header)
add_subdirectory(subproject)

# The install and package tests need the install rules, which a CPU-only
# build doesn't set up
if(NOT CMAKE_SKIP_INSTALL_RULES AND NOT HIPOBJ_CPU_RDMA)
  add_subdirectory(install)
endif()

add_subdirectory(integration)
