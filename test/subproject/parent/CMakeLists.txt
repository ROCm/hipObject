# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# A parent project that adds hipObject with add_subdirectory(), the way
# a superproject would. Configuring it fails if hipObject changes the
# parent's build type or install prefix, and building it fails if
# hipObject finds its files relative to the parent's tree instead of
# its own. Configured and built by ../run-subproject-test.cmake.

cmake_minimum_required(VERSION 3.21)

project(hipobj_subproject_parent LANGUAGES C CXX)

if(NOT HIPOBJ_SUBPROJECT_SOURCE_DIR)
  message(FATAL_ERROR "HIPOBJ_SUBPROJECT_SOURCE_DIR is not set")
endif()

set(parent_build_type "$CACHE{CMAKE_BUILD_TYPE}")
set(parent_install_prefix "$CACHE{CMAKE_INSTALL_PREFIX}")

add_subdirectory(${HIPOBJ_SUBPROJECT_SOURCE_DIR} hipobject)

if(NOT "$CACHE{CMAKE_BUILD_TYPE}" STREQUAL "${parent_build_type}")
  message(FATAL_ERROR
    "hipObject changed the parent's CMAKE_BUILD_TYPE from "
    "'${parent_build_type}' to '$CACHE{CMAKE_BUILD_TYPE}'")
endif()
if(NOT "$CACHE{CMAKE_INSTALL_PREFIX}" STREQUAL "${parent_install_prefix}")
  message(FATAL_ERROR
    "hipObject changed the parent's CMAKE_INSTALL_PREFIX from "
    "'${parent_install_prefix}' to '$CACHE{CMAKE_INSTALL_PREFIX}'")
endif()
