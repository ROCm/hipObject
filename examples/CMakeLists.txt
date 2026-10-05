# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

function(hipobj_add_example name)
  hipobj_add_executable(
    NAME ${name}
    SRCS ${ARGN})
  # Clang's -Wunsafe-buffer-usage wants std::span and the like in place of
  # pointer arithmetic, indexing argv, and C library calls such as
  # std::strtoull(). The examples are written to C++11, which has no
  # std::span, so they can't comply.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${name} PRIVATE -Wno-unsafe-buffer-usage)
  endif()
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/rdma
    ${PROJECT_SOURCE_DIR}/src/s3)
  if(HIPOBJ_CPU_RDMA)
    target_compile_definitions(${name} PRIVATE HIPOBJ_CPU_RDMA)
    target_link_libraries(${name} PRIVATE hipobj dl)
  else()
    target_link_libraries(${name} PRIVATE hipobj hip::host dl)
  endif()
  hipobj_install_executable(
    NAME ${name}
    DESTINATION bin/hipobj-examples)
endfunction()

# The programs are built here but not installed. HIPOBJInstall.cmake
# installs their sources, with CMakeLists.install.cmake as their
# CMakeLists.txt, for users to build against the installed hipObject.

hipobj_add_example(get-version get-version.cpp)
# It needs no GPU or NIC, so the tests run it
if(BUILD_TESTING)
  add_test(NAME hipobj-example-get-version
    COMMAND get-version)
endif()

find_package(CURL QUIET)

if(CURL_FOUND)
  hipobj_add_example(get-object get-object.cpp s3_curl_ops.cpp)
  hipobj_add_example(put-object put-object.cpp s3_curl_ops.cpp)
  target_compile_definitions(get-object PRIVATE HIPOBJ_HAVE_CURL)
  target_compile_definitions(put-object PRIVATE HIPOBJ_HAVE_CURL)
  target_link_libraries(get-object PRIVATE CURL::libcurl)
  target_link_libraries(put-object PRIVATE CURL::libcurl)
  message(STATUS "hipObject examples: libcurl enabled (--live mode)")
else()
  hipobj_add_example(get-object get-object.cpp)
  hipobj_add_example(put-object put-object.cpp)
  message(STATUS "hipObject examples: libcurl not found (stub mode only)")
endif()
