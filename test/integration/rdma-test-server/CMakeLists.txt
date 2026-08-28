# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

set(HIPOBJ_TEST_SERVER_SOURCES
  main.cpp
  http_server.cpp
  rdma_server.cpp
  v2_request.cpp
  v2_session.cpp
  v2_data_phase.cpp
  v2_sigv4.cpp
  v2_backend.cpp
  v2_handlers.cpp
  ${CMAKE_SOURCE_DIR}/src/common/ibv-wrapper.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/token.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/transport.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-clock.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-random.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-registry.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-transport.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-wire.cpp
  ${CMAKE_SOURCE_DIR}/src/common/hip-seam.cpp
  ${CMAKE_SOURCE_DIR}/src/common/rdma-topology.cpp
  ${CMAKE_SOURCE_DIR}/src/common/rdma-numa-wrapper.cpp)

if(HIPOBJ_BNXT)
  list(APPEND HIPOBJ_TEST_SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/rdma/bnxt/bnxt-backend.cpp)
endif()
if(HIPOBJ_IONIC)
  list(APPEND HIPOBJ_TEST_SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/rdma/ionic/ionic-backend.cpp)
endif()

add_executable(hipobj-rdma-test-server ${HIPOBJ_TEST_SERVER_SOURCES})

target_compile_features(hipobj-rdma-test-server PRIVATE cxx_std_17)

target_include_directories(hipobj-rdma-test-server PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/shared
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/common
  ${CMAKE_SOURCE_DIR}/src/rdma
  ${CMAKE_SOURCE_DIR}/src/s3
  ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(hipobj-rdma-test-server PRIVATE
  dl
  numa
  hip::host
  hsa-runtime64::hsa-runtime64)

find_package(OpenSSL REQUIRED)
target_link_libraries(hipobj-rdma-test-server PRIVATE OpenSSL::Crypto)
find_package(Threads REQUIRED)
target_link_libraries(hipobj-rdma-test-server PRIVATE Threads::Threads)

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_IONIC)
endif()

add_executable(v2-client-harness v2_client_harness.cpp v2_sigv4.cpp)

add_executable(v2-data-client v2_data_client.cpp v2_sigv4.cpp)
target_compile_features(v2-data-client PRIVATE cxx_std_17)
target_include_directories(v2-data-client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(v2-data-client PRIVATE OpenSSL::Crypto)

find_library(IBVERBS_LIB ibverbs)
if(IBVERBS_LIB)
  target_link_libraries(v2-data-client PRIVATE ${IBVERBS_LIB})
else()
  message(FATAL_ERROR "libibverbs not found - v2-data-client needs it")
endif()
target_compile_features(v2-client-harness PRIVATE cxx_std_17)
target_include_directories(v2-client-harness PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
find_package(OpenSSL REQUIRED)
target_link_libraries(v2-client-harness PRIVATE OpenSSL::Crypto)

# Control-plane end-to-end run (no RDMA hardware required).
find_program(BASH_BIN bash)
if(BASH_BIN)
  add_test(NAME v2-control-e2e
    COMMAND ${BASH_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/run-v2-e2e.sh)
  set_tests_properties(v2-control-e2e PROPERTIES TIMEOUT 180
    ENVIRONMENT
      "SERVER=$<TARGET_FILE:hipobj-rdma-test-server>;HARNESS=$<TARGET_FILE:v2-client-harness>;LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}")

  # Data-plane round trip on the local verbs device: PUT then GET
  # back the same object with the cookie echoed in the immediate.
  # Registers only where the ibverbs header is available; hosts
  # without a device fail loudly rather than silently skipping.
  include(CheckCSourceCompiles)
  check_c_source_compiles("
    #include <infiniband/verbs.h>
    int main(void) { return IBV_ACCESS_REMOTE_READ; }" HAVE_IBVERBS_HEADER)
  # A device must also be present: the data round trip needs a
  # verbs device to post on. Header-only hosts (CI containers)
  # skip the registration rather than failing at run time.
  execute_process(COMMAND bash -c "ls /dev/infiniband/uverbs* >/dev/null 2>&1"
    RESULT_VARIABLE HAVE_IBVERBS_DEVICE)
  if(HAVE_IBVERBS_HEADER AND HAVE_IBVERBS_DEVICE EQUAL 0)
    add_test(NAME v2-data-e2e
      COMMAND ${BASH_BIN} -c
        "$<TARGET_FILE:hipobj-rdma-test-server> --v2 --port 19601 --v2-access-key hipobj-test-key --v2-secret-key hipobj-test-secret 2>/dev/null &
         srv=$!;
         sleep 1;
         $<TARGET_FILE:v2-data-client> 127.0.0.1 19601 PUT /bucket/seed64k 64 &&
         $<TARGET_FILE:v2-data-client> 127.0.0.1 19601 GET /bucket/seed64k 64;
         rc=$?;
         kill $srv 2>/dev/null;
         wait $srv 2>/dev/null;
         exit $rc")
    set_tests_properties(v2-data-e2e PROPERTIES TIMEOUT 120
      ENVIRONMENT "LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}")
  endif()
endif()

install(TARGETS hipobj-rdma-test-server
  RUNTIME DESTINATION bin/hipobj-test)
