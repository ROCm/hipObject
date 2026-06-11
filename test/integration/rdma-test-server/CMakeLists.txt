# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT

set(HIPOBJ_TEST_SERVER_SOURCES
  main.cpp
  http_server.cpp
  rdma_server.cpp
  ${CMAKE_SOURCE_DIR}/src/common/ibv-wrapper.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/token.cpp
  ${CMAKE_SOURCE_DIR}/src/rdma/transport.cpp
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

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_IONIC)
endif()

install(TARGETS hipobj-rdma-test-server
  RUNTIME DESTINATION bin/hipobj-test)
