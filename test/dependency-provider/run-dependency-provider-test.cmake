# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Configure hipObject with the dependency provider in provider.cmake
#
# Usage:
#   cmake -DSETTINGS=<settings.cmake> -P run-dependency-provider-test.cmake
#
# SETTINGS is written by CMakeLists.txt in this directory.
#
# Steps:
#   1. Configure hipObject with the provider, which fails if a
#      find_package() call doesn't work with it
#   2. Check that the provider provided hip and hsa-runtime64, so the
#      test fails if the provider wasn't set

cmake_minimum_required(VERSION 3.25.2)

if(NOT SETTINGS)
  message(FATAL_ERROR "run-dependency-provider-test: SETTINGS is not set")
endif()
include(${SETTINGS})

set(build_dir ${TEST_DIR}/build)

file(REMOVE_RECURSE ${TEST_DIR})
file(MAKE_DIRECTORY ${TEST_DIR})

set(generator_args -G ${GENERATOR})
if(MAKE_PROGRAM)
  list(APPEND generator_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -S ${HIPOBJ_SOURCE_DIR} -B ${build_dir}
    ${generator_args} -C ${PROVIDER_CACHE}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "run-dependency-provider-test: configuring hipObject with the "
    "dependency provider failed:\n${output}")
endif()
message(STATUS
  "run-dependency-provider-test: configure hipObject with the "
  "dependency provider: OK")

if(NOT EXISTS ${PROVIDER_LOG})
  message(FATAL_ERROR
    "run-dependency-provider-test: the provider didn't run: there's no "
    "${PROVIDER_LOG}")
endif()
file(STRINGS ${PROVIDER_LOG} calls)
foreach(package IN ITEMS hip hsa-runtime64)
  if(NOT "provided ${package}" IN_LIST calls)
    message(FATAL_ERROR
      "run-dependency-provider-test: the provider didn't provide "
      "${package}. Its log (${PROVIDER_LOG}) has:\n${calls}")
  endif()
endforeach()
list(REMOVE_DUPLICATES calls)
list(JOIN calls "\n  " calls)
message(STATUS
  "run-dependency-provider-test: the provider handled:\n  ${calls}")
