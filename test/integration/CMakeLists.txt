# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT

option(HIPOBJ_INTEGRATION_TESTS
  "Build RC-native S3+RDMA integration test server" ON)

if(HIPOBJ_INTEGRATION_TESTS)
  add_subdirectory(rdma-test-server)
endif()

if(HIPOBJ_FIND_CUOBJECT_SERVER AND CUOBJECT_SERVER_FOUND)
  add_subdirectory(cuobject-interop)
endif()
