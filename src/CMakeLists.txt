# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# hipObject library

set(HIPOBJ_SOURCES
  hipobj.cpp
  state.cpp
  buffer.cpp
  common/hip-seam.cpp
  common/ibv-wrapper.cpp
  common/rdma-topology.cpp
  common/rdma-numa-wrapper.cpp
  rdma/transport.cpp
  rdma/token.cpp
  s3/control.cpp)

if(HIPOBJECT_V2_API)
  list(APPEND HIPOBJ_SOURCES
    rdma/v2-wire.cpp
    rdma/v2-state.cpp
    rdma/v2-clock.cpp
    rdma/v2-random.cpp
    rdma/v2-registry.cpp
    rdma/v2-transport.cpp)
endif()

if(HIPOBJ_BNXT)
  list(APPEND HIPOBJ_SOURCES
    rdma/bnxt/bnxt-backend.cpp)
endif()
if(HIPOBJ_IONIC)
  list(APPEND HIPOBJ_SOURCES
    rdma/ionic/ionic-backend.cpp)
endif()

hipobj_add_library(
  NAME hipobj
  SRCS ${HIPOBJ_SOURCES})

target_include_directories(hipobj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/hipobj>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/rdma
    ${CMAKE_CURRENT_SOURCE_DIR}/s3)

target_link_libraries(hipobj
  PUBLIC
    hip::host
    hsa-runtime64::hsa-runtime64
    stdc++
  PRIVATE
    dl)

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj
    PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj
    PRIVATE HIPOBJ_IONIC)
endif()
if(HIPOBJECT_V2_API)
  # PUBLIC: include/hipobj.h only declares the V2 API surface when this
  # macro is defined, so consumers linking hipobj::hipobj (in-tree or via
  # the exported CMake package) need it on their own translation units too.
  target_compile_definitions(hipobj
    PUBLIC HIPOBJECT_V2_API)
endif()

# The unit tests compile these same sources into a separate object
# library (see test/unit/CMakeLists.txt), so export the list to the
# parent scope as absolute paths that are valid from any directory.
list(TRANSFORM HIPOBJ_SOURCES
  PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(HIPOBJ_SOURCES ${HIPOBJ_SOURCES} PARENT_SCOPE)
