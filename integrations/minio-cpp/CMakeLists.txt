# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)

include(FetchContent)

list(APPEND CMAKE_PREFIX_PATH
  ${CMAKE_SOURCE_DIR}/cmake/minio-deps)

# minio-cpp and the dependencies we fetch for it aren't installed, so
# build them static. This is a normal variable, so it only applies to
# this directory and the projects added from it, and doesn't change the
# user's BUILD_SHARED_LIBS cache entry (which hipobj uses). minio-cpp
# requires CMake 3.10, which predates CMP0077, so without the policy
# default its option(BUILD_SHARED_LIBS) can discard the normal variable.
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

include(${CMAKE_SOURCE_DIR}/cmake/HIPOBJMinioCppDeps.cmake)
include(HIPOBJSystemIncludes)

set(MINIO_CPP_ENABLE_RDMA OFF CACHE BOOL "" FORCE)
set(MINIO_CPP_TEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  miniocpp_upstream
  GIT_REPOSITORY https://github.com/minio/minio-cpp.git
  GIT_TAG v0.4.0
  GIT_SHALLOW TRUE
)
# minio-cpp is only a build dependency of hipobj_minio, so don't
# generate its install rules. Its install(EXPORT) can't be generated
# here anyway: the curlpp and inih targets it links to are built in
# this tree (see HIPOBJMinioCppDeps.cmake) and aren't in any export
# set. CMake checks CMAKE_SKIP_INSTALL_RULES per directory, and
# EXCLUDE_FROM_ALL keeps our install script from including the
# upstream install script, which is never generated.
set(_minio_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(miniocpp_upstream)
set(CMAKE_SKIP_INSTALL_RULES "${_minio_skip_install_rules}")
set_property(DIRECTORY ${miniocpp_upstream_SOURCE_DIR}
  PROPERTY EXCLUDE_FROM_ALL TRUE)
hipobj_mark_dir_targets_system(${miniocpp_upstream_SOURCE_DIR})
# minio-cpp adds its own -Wall -Wextra -Wconversion to miniocpp, which
# warn under Clang. It isn't our code to fix, so silence all warnings
# in its sources. This only affects compiling miniocpp itself, not
# targets that link it.
target_compile_options(miniocpp PRIVATE -w)

hipobj_add_library(
  NAME hipobj_minio
  TYPE STATIC
  SRCS
    src/rdma.cpp
    src/client.cpp)

if(HIPOBJECT_V2_API)
  target_compile_definitions(hipobj_minio PRIVATE HIPOBJECT_V2_API)
endif()

target_include_directories(hipobj_minio
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(hipobj_minio
  PUBLIC
    miniocpp::miniocpp
    hipobj
    hip::host)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()

hipobj_add_executable(
  NAME minio-getput-rdma
  SRCS examples/getput-rdma.cpp)

target_link_libraries(minio-getput-rdma PRIVATE
  hipobj_minio
  hipobj
  hip::host
  miniocpp::miniocpp)

# hipobj_minio is build-only. Its headers include minio-cpp's headers
# and it links minio-cpp publicly, but minio-cpp and the curlpp and
# inih libraries we build for it aren't installed, so an installed
# archive and headers would be unusable. The example links those
# statically, so it only needs libhipobj at run time when hipobj is a
# shared library.
hipobj_install_executable(
  NAME minio-getput-rdma
  DESTINATION bin/hipobj-examples)
