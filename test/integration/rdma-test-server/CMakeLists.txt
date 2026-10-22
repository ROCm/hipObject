# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

set(HIPOBJ_TEST_SERVER_SOURCES
  main.cpp
  http_server.cpp
  rdma_server.cpp
  ${PROJECT_SOURCE_DIR}/src/common/ibv-wrapper.cpp
  ${PROJECT_SOURCE_DIR}/src/rdma/token.cpp
  ${PROJECT_SOURCE_DIR}/src/rdma/transport.cpp
  ${PROJECT_SOURCE_DIR}/src/common/hip-seam.cpp
  ${PROJECT_SOURCE_DIR}/src/common/rdma-topology.cpp)

if(HIPOBJ_BNXT)
  list(APPEND HIPOBJ_TEST_SERVER_SOURCES
    ${PROJECT_SOURCE_DIR}/src/rdma/bnxt/bnxt-backend.cpp)
endif()
if(HIPOBJ_IONIC)
  list(APPEND HIPOBJ_TEST_SERVER_SOURCES
    ${PROJECT_SOURCE_DIR}/src/rdma/ionic/ionic-backend.cpp)
endif()

hipobj_add_test_executable(
  NAME hipobj-rdma-test-server
  SRCS ${HIPOBJ_TEST_SERVER_SOURCES})

target_include_directories(hipobj-rdma-test-server PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/shared
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/common
  ${PROJECT_SOURCE_DIR}/src/rdma
  ${PROJECT_SOURCE_DIR}/src/s3
  ${PROJECT_SOURCE_DIR}/test/common
  ${CMAKE_CURRENT_SOURCE_DIR})

if(HIPOBJ_CPU_RDMA)
target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_CPU_RDMA)
target_link_libraries(hipobj-rdma-test-server PRIVATE
  dl
  numa)
else()
target_link_libraries(hipobj-rdma-test-server PRIVATE
  dl
  hip::host
  hsa-runtime64::hsa-runtime64)
endif()

find_package(Threads REQUIRED)
target_link_libraries(hipobj-rdma-test-server PRIVATE Threads::Threads)

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj-rdma-test-server PRIVATE HIPOBJ_IONIC)
endif()

# The tests component isn't packaged
install(TARGETS hipobj-rdma-test-server
  RUNTIME DESTINATION bin/hipobj-test
  COMPONENT tests)
