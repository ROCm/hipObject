# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)

include(FetchContent)

list(APPEND CMAKE_PREFIX_PATH
  ${CMAKE_SOURCE_DIR}/cmake/minio-deps)

include(${CMAKE_SOURCE_DIR}/cmake/HIPOBJMinioCppDeps.cmake)

set(MINIO_CPP_ENABLE_RDMA OFF CACHE BOOL "" FORCE)
set(MINIO_CPP_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  miniocpp_upstream
  GIT_REPOSITORY https://github.com/minio/minio-cpp.git
  GIT_TAG v0.4.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniocpp_upstream)

add_library(hipobj_minio STATIC
  src/rdma.cpp
  src/client.cpp)

target_compile_features(hipobj_minio PUBLIC cxx_std_17)

target_include_directories(hipobj_minio
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/hipobj_minio>
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

add_executable(minio-getput-rdma
  examples/getput-rdma.cpp)

set_source_files_properties(
  examples/getput-rdma.cpp
  PROPERTIES LANGUAGE HIP)

target_compile_features(minio-getput-rdma PUBLIC cxx_std_17)
target_link_libraries(minio-getput-rdma PRIVATE
  hipobj_minio
  hipobj
  hip::host
  miniocpp::miniocpp)

install(TARGETS hipobj_minio minio-getput-rdma
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin/hipobj-examples)

install(DIRECTORY include/hipobj_minio
  DESTINATION include)
