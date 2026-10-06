# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)

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

# Declared out here, ahead of the HIPOBJ_DOCS_ONLY guard,
# because the documentation module reads it on every
# configure path: include/hipobj.h gates the v2 API
# declarations on this macro, so Doxygen has to agree with
# the library about whether that surface exists.
option(HIPOBJECT_V2_API
  "Build the hipobj-rc-v2 control protocol" ON)

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
    LANGUAGES C CXX)
endif()

# Default to a Debug build. This has to come after project(), since
# that's where CMake decides whether the generator is multi-config.
# Multi-config generators (Ninja Multi-Config, Xcode, etc.) pick the
# configuration at build time and don't use CMAKE_BUILD_TYPE. For
# single-config generators, project() creates an empty cache entry,
# so FORCE is needed to replace it.
get_property(HIPOBJ_IS_MULTI_CONFIG GLOBAL
  PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT HIPOBJ_IS_MULTI_CONFIG)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING
      "Build type" FORCE)
  endif()
  set_property(CACHE CMAKE_BUILD_TYPE
    PROPERTY STRINGS
    "Debug" "Release" "RelWithDebInfo" "None")
endif()

list(APPEND CMAKE_MODULE_PATH
  ${CMAKE_SOURCE_DIR}/cmake)

if(NOT HIPOBJ_DOCS_ONLY)

include(HIPOBJUseROCmCMake)
include(HIPOBJSanitizers)
include(HIPOBJClangTidy)
include(HIPOBJAddLibraries)
include(HIPOBJAddExecutable)
include(HIPOBJFetchCuObject)

set(HIPOBJ_CXX_STANDARD "17" CACHE STRING
  "C++ standard to build with")
set_property(CACHE HIPOBJ_CXX_STANDARD
  PROPERTY STRINGS "17" "20" "23")

option(BUILD_SHARED_LIBS
  "Build using shared libraries" OFF)

include(CTest)

find_package(hip CONFIG REQUIRED)
message(STATUS "hip found @ ${hip_DIR}")

find_package(hsa-runtime64 CONFIG REQUIRED)
message(STATUS
  "hsa-runtime64 @ ${hsa-runtime64_DIR}")

option(HIPOBJ_BNXT
  "Build BNXT (Thor-2) RDMA provider" ON)
option(HIPOBJ_IONIC
  "Build IONIC (Pensando) RDMA provider" ON)

add_subdirectory(src)

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
