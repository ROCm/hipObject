# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# A C-only program built against the installed hipObject package the
# way a consumer would build it. It doesn't find HIP or any other
# hipObject dependency itself, so the package has to provide everything.
# Configured and run by ../run-install-test.cmake.

cmake_minimum_required(VERSION 3.21)

project(hipobj_install_consumer LANGUAGES C)

if(NOT HIPOBJ_EXPECTED_VERSION)
  message(FATAL_ERROR "HIPOBJ_EXPECTED_VERSION is not set")
endif()

find_package(hipobj ${HIPOBJ_EXPECTED_VERSION} EXACT CONFIG REQUIRED)

add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE hipobj::hipobj)
target_compile_definitions(consumer PRIVATE
  HIPOBJ_EXPECTED_VERSION="${HIPOBJ_EXPECTED_VERSION}")

enable_testing()
add_test(NAME consumer COMMAND consumer)
