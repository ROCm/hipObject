# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Unit tests link a separate build of the library objects with
# HIPOBJ_UNIT_TESTS defined. The macro only opens test accessors
# (IBVWrapper::funcsForTest); compiling the sources twice into two
# distinct targets avoids ODR issues, and the shipped hipobj library
# is compiled without the macro so the accessors are absent there.

# HIPOBJ_SOURCES is exported by src/CMakeLists.txt as absolute paths.
hipobj_add_library(
  NAME hipobj_test_objects
  TYPE OBJECT
  SRCS ${HIPOBJ_SOURCES})

target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJ_UNIT_TESTS)

# Compiled only for the tests, so compiled with -pthread like them (see
# hipobj_add_test_executable()). NEVER REMOVE IT.
target_compile_options(hipobj_test_objects PRIVATE -pthread)

if(HIPOBJECT_V2_API)
  # Server-side v2 sources for the session/handler unit tests.
  set(HIPOBJ_V2_SERVER_SOURCES
    ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server/v2_request.cpp
    ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server/v2_session.cpp
    ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server/v2_handlers.cpp
    ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server/v2_data_phase.cpp
    ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server/v2_backend.cpp)

  hipobj_add_library(
    NAME hipobj_v2_server_objects
    TYPE OBJECT
    SRCS ${HIPOBJ_V2_SERVER_SOURCES})
  target_include_directories(hipobj_v2_server_objects
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
      ${PROJECT_SOURCE_DIR}/shared
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/common
      ${PROJECT_SOURCE_DIR}/src/rdma
      ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server)
  # Like hipobj_test_objects. NEVER REMOVE IT.
  target_compile_options(hipobj_v2_server_objects PRIVATE -pthread)
endif()

target_include_directories(hipobj_test_objects
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/shared
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/rdma
    ${PROJECT_SOURCE_DIR}/src/s3)

target_link_libraries(hipobj_test_objects
  PUBLIC
    hip::host
    hsa-runtime64::hsa-runtime64
    stdc++
    Threads::Threads)

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJ_IONIC)
endif()
if(HIPOBJECT_V2_API)
  target_compile_definitions(hipobj_test_objects PRIVATE HIPOBJECT_V2_API)
endif()

# A test of the public API that runs against the fake RDMA device and HIP
# runtime in fake-device.h
function(hipobj_add_fake_device_test TEST_NAME TEST_SOURCE)
  hipobj_add_test(${TEST_NAME} ${TEST_SOURCE})
  target_sources(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake-device.cpp)
endfunction()

hipobj_add_test(test-rdma-token
  test-rdma-token.cpp)

hipobj_add_test(test-parse-number
  test-parse-number.cpp)

# The MinIO bridge's header parsing doesn't need minio-cpp, so it's tested
# in every build, not only with HIPOBJ_MINIO_CLIENT
hipobj_add_test(test-minio-bytes-transferred
  test-minio-bytes-transferred.cpp)
target_include_directories(test-minio-bytes-transferred PRIVATE
  ${PROJECT_SOURCE_DIR}/integrations/minio-cpp/src)

hipobj_add_test(test-vendor-ops
  test-vendor-ops.cpp)

# Checks ibv-core.h against <infiniband/verbs.h>, so it's only built
# where the rdma-core headers are installed. They have to be rdma-core
# 49 or later, which added the newest member ibv-core.h declares,
# ibv_port_attr's active_speed_ex.
include(CheckCSourceCompiles)
check_c_source_compiles("
  #include <infiniband/verbs.h>
  int main(void) { struct ibv_port_attr attr = {0}; return (int)attr.active_speed_ex; }"
  HIPOBJ_HAVE_IBVERBS_49_HEADER)
if(HIPOBJ_HAVE_IBVERBS_49_HEADER)
  hipobj_add_test(test-ibv-layout
    test-ibv-layout.cpp)
  # ibv-layout-real.c is C so that it can include <infiniband/verbs.h>
  # alongside the C++ code that includes ibv-core.h
  target_sources(test-ibv-layout PRIVATE
    ibv-layout-real.c)
  set_target_properties(test-ibv-layout PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
else()
  message(STATUS
    "rdma-core 49 or later headers not found; not building test-ibv-layout")
endif()

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

if(HIPOBJECT_V2_API)
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

  hipobj_add_test(test-v2-connref
    test-v2-connref.cpp)
endif()

hipobj_add_test(test-seams
  test-seams.cpp)

hipobj_add_test(test-replace-by-move
  test-replace-by-move.cpp)

hipobj_add_test(test-gid-select
  test-gid-select.cpp)

hipobj_add_test(test-buffer-registration
  test-buffer-registration.cpp)

hipobj_add_fake_device_test(test-api-args
  test-api-args.cpp)

hipobj_add_fake_device_test(test-api-exceptions
  test-api-exceptions.cpp)

hipobj_add_fake_device_test(test-transport
  test-transport.cpp)

hipobj_add_test(test-s3-control
  test-s3-control.cpp)

hipobj_add_fake_device_test(test-staging
  test-staging.cpp)
# hipObject reads these settings once and caches them, so the tests of
# them run in a process of their own (see test-staging.cpp)
add_test(NAME test-staging-env
  COMMAND test-staging --gtest_filter=StagingEnvTest.*)
set_tests_properties(test-staging-env PROPERTIES
  ENVIRONMENT "HIPOBJ_STAGE_TIMEOUT_MS=20;HIPOBJ_REQUIRE_GPU_DIRECT=1")
add_test(NAME test-staging-bad-env
  COMMAND test-staging --gtest_filter=StagingBadEnvTest.*)
set_tests_properties(test-staging-bad-env PROPERTIES
  ENVIRONMENT "HIPOBJ_STAGE_TIMEOUT_MS=+20")

hipobj_add_test(test-thread-safety
  test-thread-safety.cpp)
# A deadlock fails the test instead of hanging the run
set_tests_properties(test-thread-safety PROPERTIES TIMEOUT 120)
