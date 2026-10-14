# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
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

# HIPOBJ_AWS_S3_CLIENT is declared in the top-level CMakeLists.txt (it
# also gates a BUILD_TESTING-independent add_subdirectory() there).
if(HIPOBJ_AWS_S3_CLIENT)
  add_subdirectory(aws-s3-client)
endif()
