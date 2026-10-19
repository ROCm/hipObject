# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# TheRock requires 3.25. 3.25.0 and 3.25.1 pass FetchContent_Declare()'s
# SYSTEM on to ExternalProject_Add() as part of the previous option's
# value, which breaks the download.
cmake_minimum_required(VERSION 3.25.2)

# The version is set only in include/hipobj.h, by the HIPOBJ_VERSION_*
# macros, and read from there. The installed library reports the
# header's version too, and the hipobj-install test checks that it's
# this one. docs/conf.py reads the same lines.
set(HIPOBJ_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/hipobj.h")
file(STRINGS "${HIPOBJ_VERSION_HEADER}" HIPOBJ_VERSION_LINES
  REGEX "^#define[ \t]+HIPOBJ_VERSION_(MAJOR|MINOR|PATCH)[ \t]")
foreach(part IN ITEMS MAJOR MINOR PATCH)
  string(REGEX MATCH
    "#define[ \t]+HIPOBJ_VERSION_${part}[ \t]+([0-9]+)[ \t]*(;|$)"
    match "${HIPOBJ_VERSION_LINES}")
  # Not if(NOT CMAKE_MATCH_1): CMake treats a version number of 0 as false
  if(match STREQUAL "")
    message(FATAL_ERROR
      "HIPOBJ_VERSION_${part} isn't defined as a number in "
      "${HIPOBJ_VERSION_HEADER}")
  endif()
  set(HIPOBJ_VERSION_${part} "${CMAKE_MATCH_1}")
endforeach()
# Reconfigure when the header changes, so that a new version takes
# effect without anyone having to rerun CMake
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${HIPOBJ_VERSION_HEADER}")
set(HIPOBJ_LIBRARY_VERSION
  "${HIPOBJ_VERSION_MAJOR}.${HIPOBJ_VERSION_MINOR}.${HIPOBJ_VERSION_PATCH}")

# The shared library's soname has only the major version
# (libhipobj.so.<major>), so a program linked against one release loads
# any later release with the same major version. A minor or patch release
# must NEVER break the ABI, even in a way that keeps the API
# source-compatible, such as reordering a struct's fields or changing an
# enumerator's value. CMake would otherwise put the whole version in the
# soname.
#
# While the major version is 0, hipObject is in development, and its ABI
# changes between minor versions without a change to the soname.
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
    LANGUAGES C CXX)
endif()

# Default to a RelWithDebInfo build. This has to come after project(),
# since that's where CMake decides whether the generator is
# multi-config. Multi-config generators (Ninja Multi-Config, Xcode,
# etc.) pick the configuration at build time and don't use
# CMAKE_BUILD_TYPE. For single-config generators, project() creates an
# empty cache entry, so FORCE is needed to replace it. When hipObject is
# a subproject, the build type is the superproject's to choose.
get_property(HIPOBJ_IS_MULTI_CONFIG GLOBAL
  PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND NOT HIPOBJ_IS_MULTI_CONFIG)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
      "Build type" FORCE)
  endif()
  set_property(CACHE CMAKE_BUILD_TYPE
    PROPERTY STRINGS
    "Debug" "Release" "RelWithDebInfo" "MinSizeRel" "None")
endif()

list(APPEND CMAKE_MODULE_PATH
  ${PROJECT_SOURCE_DIR}/cmake)

if(NOT HIPOBJ_DOCS_ONLY)

set(HIPOBJ_CXX_STANDARD "20" CACHE STRING
  "C++ standard to build with")
set_property(CACHE HIPOBJ_CXX_STANDARD
  PROPERTY STRINGS "20" "23" "26")

# CMake knows how to ask Clang for C++26 from 3.27 on, and GCC from
# 3.30, both newer than the version hipObject otherwise needs. This is
# checked before ROCm is looked for, and rocm-cmake possibly
# downloaded, so it's the first error reported.
if(HIPOBJ_CXX_STANDARD STREQUAL "26"
    AND CMAKE_VERSION VERSION_LESS 3.30)
  message(FATAL_ERROR
    "HIPOBJ_CXX_STANDARD=26 needs CMake 3.30 or later, but this is "
    "CMake ${CMAKE_VERSION}")
endif()

# This checks the sanitizer options, and tests the compilers with them,
# before ROCm is looked for, so that a problem with them is reported
# early
include(HIPOBJSanitizers)

# Put the compiler's implicit include directories in
# compile_commands.json, so tools that read it, such as clangd and
# clang-tidy, search the same system headers as the compiler instead
# of guessing (for example, when the build uses GCC). They're appended,
# to keep any that a toolchain file or parent project set. This has to
# come before rocm-cmake is loaded (HIPOBJUseROCmCMake), which warns
# about any change to these variables after that.
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  foreach(lang IN ITEMS C CXX)
    list(APPEND CMAKE_${lang}_STANDARD_INCLUDE_DIRECTORIES
      ${CMAKE_${lang}_IMPLICIT_INCLUDE_DIRECTORIES})
    list(REMOVE_DUPLICATES CMAKE_${lang}_STANDARD_INCLUDE_DIRECTORIES)
  endforeach()
endif()

# CMAKE_PREFIX_PATH as hipObject received it, before HIPOBJUseROCmCMake
# adds ROCm's prefixes. It includes any prefixes a project above
# hipObject added as a normal variable, which $CACHE{CMAKE_PREFIX_PATH}
# doesn't. The subproject test passes it on to its parent project.
set(HIPOBJ_INHERITED_PREFIX_PATH "${CMAKE_PREFIX_PATH}")

include(HIPOBJUseROCmCMake)
include(HIPOBJCoverage)
include(HIPOBJClangTidy)
include(HIPOBJIWYU)
include(HIPOBJAddLibraries)
include(HIPOBJAddExecutable)
include(HIPOBJFetchCuObject)

option(BUILD_SHARED_LIBS
  "Build using shared libraries" ON)

include(CTest)

find_package(hip CONFIG REQUIRED)
message(STATUS "hip found @ ${hip_DIR}")

find_package(hsa-runtime64 CONFIG REQUIRED)
message(STATUS
  "hsa-runtime64 @ ${hsa-runtime64_DIR}")

# The library uses std::mutex and std::this_thread. Where the C library
# needs it (glibc before 2.34, for example), Threads::Threads adds
# -pthread rather than just -lpthread. With glibc 2.34 and later, which
# includes the pthread functions, it adds nothing.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

option(HIPOBJ_BNXT
  "Build BNXT (Thor-2) RDMA provider" ON)
option(HIPOBJ_IONIC
  "Build IONIC (Pensando) RDMA provider" ON)

add_subdirectory(src)

# Before the tests, which build the installed examples and run the
# installed tools
option(HIPOBJ_INSTALL_EXAMPLES
  "Build the examples and install their sources" ON)
option(HIPOBJ_INSTALL_TOOLS
  "Build and install the tool programs" ON)

if(BUILD_TESTING)
  include(HIPOBJUseGTest)
  add_subdirectory(test)
endif()

if(HIPOBJ_INSTALL_EXAMPLES)
  add_subdirectory(examples)
endif()

if(HIPOBJ_INSTALL_TOOLS)
  add_subdirectory(tools)
endif()

option(HIPOBJ_MINIO_CLIENT
  "Build minio-cpp RDMA bridge (integrations/minio-cpp)" OFF)
if(HIPOBJ_MINIO_CLIENT)
  add_subdirectory(integrations/minio-cpp)
endif()

include(HIPOBJInstall)

endif() # NOT HIPOBJ_DOCS_ONLY

include(HIPOBJDocumentation)
