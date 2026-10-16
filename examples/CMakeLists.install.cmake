# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Builds the hipObject examples against an installed hipObject (see
# README.md). hipObject installs this file as the examples'
# CMakeLists.txt. The CMakeLists.txt in hipObject's source tree builds
# them with hipObject's own build settings instead.
#
# Add -DCMAKE_PREFIX_PATH=/opt/rocm/core-<major>.<minor> (the ROCm and
# hipObject install prefix) if CMake doesn't find hip or hipobj.

cmake_minimum_required(VERSION 3.25.2)

project(hipobj_examples LANGUAGES CXX)

find_package(hip CONFIG REQUIRED)
find_package(hipobj CONFIG REQUIRED)

# Without libcurl, the examples only have their stub mode
find_package(CURL)

foreach(example IN ITEMS get-object put-object)
  add_executable(${example} ${example}.cpp)
  # The examples are written to C++11
  set_target_properties(${example} PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  target_compile_options(${example} PRIVATE -Wall -Wextra)
  target_link_libraries(${example} PRIVATE hipobj::hipobj hip::host)
  if(CURL_FOUND)
    target_sources(${example} PRIVATE s3_curl_ops.cpp)
    target_compile_definitions(${example} PRIVATE HIPOBJ_HAVE_CURL)
    target_link_libraries(${example} PRIVATE CURL::libcurl)
  endif()
endforeach()
