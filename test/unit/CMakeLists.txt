# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

hipobj_add_test(test-rdma-token
  test-rdma-token.cpp)

hipobj_add_test(test-vendor-ops
  test-vendor-ops.cpp)

find_package(CURL QUIET)
if(CURL_FOUND)
  find_package(Threads REQUIRED)
  hipobj_add_test(test-s3-curl-ops
    test-s3-curl-ops.cpp)
  target_sources(test-s3-curl-ops PRIVATE
    ${PROJECT_SOURCE_DIR}/examples/s3_curl_ops.cpp)
  target_include_directories(test-s3-curl-ops PRIVATE
    ${PROJECT_SOURCE_DIR}/examples)
  target_link_libraries(test-s3-curl-ops PRIVATE
    CURL::libcurl
    Threads::Threads)
  set_tests_properties(test-s3-curl-ops PROPERTIES TIMEOUT 60)
endif()
