# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include(${CMAKE_SOURCE_DIR}/cmake/HIPOBJUseGTest.cmake)

hipobj_add_test(test-rdma-header test-rdma-header.cpp)
target_link_libraries(test-rdma-header PRIVATE hipobj_minio)

hipobj_add_test(test-bridge-bounds test-bridge-bounds.cpp)
target_link_libraries(test-bridge-bounds PRIVATE hipobj_minio)
target_include_directories(test-bridge-bounds PRIVATE
  ${CMAKE_SOURCE_DIR}/integrations/minio-cpp/include)
