# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

set(HIPOBJ_TEST_SERVER_SOURCES
  main.cpp
  http_server.cpp
  rdma_server.cpp
  v2_request.cpp
  v2_session.cpp
  v2_sigv4.cpp
  v2_backend.cpp
  v2_handlers.cpp
  ${CMAKE_SOURCE_DIR}/src/common/ibv-wrapper.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/token.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/transport.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-clock.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-random.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/v2-registry.cpp
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
target_compile_features(v2-client-harness PRIVATE cxx_std_17)
target_include_directories(v2-client-harness PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
find_package(OpenSSL REQUIRED)
target_link_libraries(v2-client-harness PRIVATE OpenSSL::Crypto)

install(TARGETS hipobj-rdma-test-server
  RUNTIME DESTINATION bin/hipobj-test)
