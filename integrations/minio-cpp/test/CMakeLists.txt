# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include(${CMAKE_SOURCE_DIR}/cmake/HIPOBJUseGTest.cmake)

# Don't link hipobj_minio here. hipobj_add_test() already links the
# hipobj_test_objects copy of the library sources, and hipobj_minio
# links hipobj, so a shared hipobj would put a second copy of every
# library global in the process.
hipobj_add_test(test-rdma-header test-rdma-header.cpp)
