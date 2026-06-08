# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include(${CMAKE_SOURCE_DIR}/cmake/HIPOBJUseGTest.cmake)

hipobj_add_test(test-rdma-header test-rdma-header.cpp)
target_link_libraries(test-rdma-header PRIVATE hipobj_minio)
