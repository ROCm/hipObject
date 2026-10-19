# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# A parent project that adds hipObject with add_subdirectory(), the way
# a superproject would. Configuring it fails if hipObject changes the
# parent's build type or install prefix, or if hipObject's own
# subproject test wouldn't pass on a prefix the parent added to
# CMAKE_PREFIX_PATH. Building it fails if hipObject finds its files
# relative to the parent's tree instead of its own. Configured and
# built by ../run-subproject-test.cmake.

cmake_minimum_required(VERSION 3.25.2)

project(hipobj_subproject_parent LANGUAGES C CXX)

if(NOT HIPOBJ_SUBPROJECT_SOURCE_DIR)
  message(FATAL_ERROR "HIPOBJ_SUBPROJECT_SOURCE_DIR is not set")
endif()

set(parent_build_type "$CACHE{CMAKE_BUILD_TYPE}")
set(parent_install_prefix "$CACHE{CMAKE_INSTALL_PREFIX}")

# A prefix that hipObject only gets as a normal variable, not from the
# cache. It doesn't need to exist.
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_BINARY_DIR}/parent-prefix")

add_subdirectory(${HIPOBJ_SUBPROJECT_SOURCE_DIR} hipobject)

# A superproject links the library by the installed package's name
if(NOT TARGET hipobj::hipobj)
  message(FATAL_ERROR "hipObject doesn't define the hipobj::hipobj target")
endif()

# hipObject's subproject test has to pass on CMAKE_PREFIX_PATH as
# hipObject received it: with the prefix above, and without the ROCm
# prefixes hipObject adds itself
set(nested_cache
  "${CMAKE_CURRENT_BINARY_DIR}/hipobject/test/subproject/parent-cache.cmake")
if(NOT EXISTS "${nested_cache}")
  message(FATAL_ERROR
    "hipObject's subproject test didn't write ${nested_cache}")
endif()
file(READ "${nested_cache}" nested_cache_entries)
if(NOT nested_cache_entries MATCHES
    "set\\(CMAKE_PREFIX_PATH \"([^\"]*)\" CACHE")
  message(FATAL_ERROR
    "${nested_cache} doesn't set CMAKE_PREFIX_PATH")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL "${CMAKE_PREFIX_PATH}")
  message(FATAL_ERROR
    "hipObject's subproject test passes on CMAKE_PREFIX_PATH "
    "'${CMAKE_MATCH_1}', but hipObject received '${CMAKE_PREFIX_PATH}'")
endif()

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
