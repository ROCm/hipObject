# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_BUILD_TYPE
    AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING
    "Build type")
endif()

set(HIPOBJ_VERSION_MAJOR 0)
set(HIPOBJ_VERSION_MINOR 1)
set(HIPOBJ_VERSION_PATCH 0)
set(HIPOBJ_LIBRARY_VERSION
  "${HIPOBJ_VERSION_MAJOR}.${HIPOBJ_VERSION_MINOR}.${HIPOBJ_VERSION_PATCH}"
  CACHE STRING "hipObject library version")
set(HIPOBJ_LIBRARY_SOVERSION
  "${HIPOBJ_VERSION_MAJOR}")

option(HIPOBJ_DOCS_ONLY
  "Only configure documentation targets (no HIP/ROCm)"
  OFF)

if(HIPOBJ_DOCS_ONLY)
  project(hipobj
    VERSION ${HIPOBJ_LIBRARY_VERSION}
    DESCRIPTION
      "RDMA-accelerated S3 object storage for GPUs"
    LANGUAGES CXX)
else()
  project(hipobj
    VERSION ${HIPOBJ_LIBRARY_VERSION}
    DESCRIPTION
      "RDMA-accelerated S3 object storage for GPUs"
    LANGUAGES C CXX HIP)
endif()

list(APPEND CMAKE_MODULE_PATH
  ${CMAKE_SOURCE_DIR}/cmake)

if(NOT HIPOBJ_DOCS_ONLY)

include(HIPOBJUseROCmCMake)
include(HIPOBJCompilerOptions)
include(HIPOBJSanitizers)
include(HIPOBJClangTidy)
include(HIPOBJFetchCuObject)

option(BUILD_SHARED_LIBS
  "Build using shared libraries" OFF)
set_property(CACHE CMAKE_BUILD_TYPE
  PROPERTY STRINGS
  "Debug" "Release" "RelWithDebInfo" "None")

include(CTest)

find_package(hip CONFIG REQUIRED)
message(STATUS "hip found @ ${hip_DIR}")

find_package(hsa-runtime64 CONFIG REQUIRED)
message(STATUS
  "hsa-runtime64 @ ${hsa-runtime64_DIR}")

option(HIPOBJ_BNXT
  "Build BNXT (Thor-2) RDMA provider" ON)
option(HIPOBJ_IONIC
  "Build IONIC (Pensando) RDMA provider" OFF)

set(OFFLOAD_ARCH "" CACHE STRING
  "GPU arch (e.g. gfx942)")
if(NOT OFFLOAD_ARCH)
  find_program(ROCMINFO rocminfo
    PATHS ${ROCM_PATH}/bin /opt/rocm/bin
    ENV PATH)
  if(ROCMINFO)
    execute_process(
      COMMAND ${ROCMINFO}
      OUTPUT_VARIABLE ROCMINFO_OUTPUT
      RESULT_VARIABLE ROCMINFO_RESULT)
    if(ROCMINFO_RESULT EQUAL 0)
      string(REGEX MATCH "gfx[0-9a-f]+"
        DETECTED_ARCH "${ROCMINFO_OUTPUT}")
      if(DETECTED_ARCH)
        set(OFFLOAD_ARCH ${DETECTED_ARCH})
      endif()
    endif()
  endif()
endif()

set(HIPOBJ_SOURCES
  src/hipobj.cpp
  src/state.cpp
  src/buffer.cpp
  src/common/ibv-wrapper.cpp
  src/common/rdma-topology.cpp
  src/common/rdma-numa-wrapper.cpp
  src/rdma/transport.cpp
  src/rdma/token.cpp
  src/s3/control.cpp)

if(HIPOBJ_BNXT)
  list(APPEND HIPOBJ_SOURCES
    src/rdma/bnxt/bnxt-backend.cpp)
endif()
if(HIPOBJ_IONIC)
  list(APPEND HIPOBJ_SOURCES
    src/rdma/ionic/ionic-backend.cpp)
endif()

set_source_files_properties(
  ${HIPOBJ_SOURCES}
  PROPERTIES LANGUAGE HIP)

add_library(hipobj STATIC ${HIPOBJ_SOURCES})

target_include_directories(hipobj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/hipobj>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/rdma
    ${CMAKE_SOURCE_DIR}/src/s3)

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

if(BUILD_TESTING)
  include(HIPOBJUseGTest)
  add_subdirectory(test)
endif()

option(HIPOBJ_INSTALL_EXAMPLES
  "Install example programs" ON)
if(HIPOBJ_INSTALL_EXAMPLES)
  add_subdirectory(examples)
endif()

option(HIPOBJ_MINIO_CLIENT
  "Build minio-cpp RDMA bridge (integrations/minio-cpp)" OFF)
if(HIPOBJ_MINIO_CLIENT)
  add_subdirectory(integrations/minio-cpp)
endif()

include(HIPOBJInstall)

endif() # NOT HIPOBJ_DOCS_ONLY

include(HIPOBJDocumentation)
