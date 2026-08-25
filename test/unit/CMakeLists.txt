# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Unit tests link a separate build of the library objects with
# HIPOBJ_UNIT_TESTS defined. The macro only opens test accessors
# (IBVWrapper::funcsForTest); compiling the sources twice into two
# distinct targets avoids ODR issues, and the shipped hipobj library
# is compiled without the macro so the accessors are absent there.

# HIPOBJ_SOURCES uses root-relative paths; rebase them for this
# directory scope.
set(HIPOBJ_TEST_SOURCES)
foreach(SRC IN LISTS HIPOBJ_SOURCES)
  list(APPEND HIPOBJ_TEST_SOURCES ${CMAKE_SOURCE_DIR}/${SRC})
endforeach()

add_library(hipobj_test_objects OBJECT ${HIPOBJ_TEST_SOURCES})

# Server-side v2 sources for the session/handler unit tests.
set(HIPOBJ_V2_SERVER_SOURCES
  ${CMAKE_SOURCE_DIR}/test/integration/rdma-test-server/v2_request.cpp
  ${CMAKE_SOURCE_DIR}/test/integration/rdma-test-server/v2_session.cpp)

target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJ_UNIT_TESTS)

add_library(hipobj_v2_server_objects OBJECT ${HIPOBJ_V2_SERVER_SOURCES})
target_compile_features(hipobj_v2_server_objects PRIVATE cxx_std_17)
target_include_directories(hipobj_v2_server_objects
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/rdma
    ${CMAKE_SOURCE_DIR}/test/integration/rdma-test-server)

target_include_directories(hipobj_test_objects
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/rdma
    ${CMAKE_SOURCE_DIR}/src/s3)

target_link_libraries(hipobj_test_objects
  PUBLIC
    hip::host
    hsa-runtime64::hsa-runtime64
    stdc++)

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJ_IONIC)
endif()

set_source_files_properties(
  ${HIPOBJ_TEST_SOURCES}
  PROPERTIES LANGUAGE HIP)

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

hipobj_add_test(test-v2-wire
  test-v2-wire.cpp)

hipobj_add_test(test-v2-state
  test-v2-state.cpp)

hipobj_add_test(test-v2-registry
  test-v2-registry.cpp)

hipobj_add_test(test-v2-random
  test-v2-random.cpp)

hipobj_add_test(test-v2-conflict
  test-v2-conflict.cpp)

hipobj_add_test(test-v2-server
  test-v2-server.cpp)

hipobj_add_test(test-seams
  test-seams.cpp)
