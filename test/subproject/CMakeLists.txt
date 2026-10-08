# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build hipObject as a subproject of another CMake project (see
# run-subproject-test.cmake). This catches CMake code that assumes
# hipObject is the top-level project, which nothing else here does.

# The parent project builds hipObject with this build's compilers,
# options, and dependencies. It doesn't build the tests or examples,
# which would make the test take much longer.
set(HIPOBJ_SUBPROJECT_CACHE ${CMAKE_CURRENT_BINARY_DIR}/parent-cache.cmake)
string(CONCAT HIPOBJ_SUBPROJECT_CACHE_ENTRIES
  "set(HIPOBJ_SUBPROJECT_SOURCE_DIR \"${PROJECT_SOURCE_DIR}\" CACHE PATH \"\")\n"
  "set(CMAKE_PREFIX_PATH \"$CACHE{CMAKE_PREFIX_PATH}\" CACHE STRING \"\")\n"
  "set(BUILD_TESTING OFF CACHE BOOL \"\")\n"
  "set(HIPOBJ_INSTALL_EXAMPLES OFF CACHE BOOL \"\")\n")
foreach(var IN ITEMS
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    ROCM_PATH
    ROCM_VERSION
    hip_DIR
    hsa-runtime64_DIR
    ROCmCMakeBuildTools_DIR
    BUILD_SHARED_LIBS
    HIPOBJ_CXX_STANDARD
    HIPOBJECT_V2_API
    HIPOBJ_BNXT
    HIPOBJ_IONIC)
  string(APPEND HIPOBJ_SUBPROJECT_CACHE_ENTRIES
    "set(${var} \"${${var}}\" CACHE STRING \"\")\n")
endforeach()
file(WRITE ${HIPOBJ_SUBPROJECT_CACHE} "${HIPOBJ_SUBPROJECT_CACHE_ENTRIES}")

set(HIPOBJ_SUBPROJECT_TEST_SETTINGS
  ${CMAKE_CURRENT_BINARY_DIR}/subproject-test-settings.cmake)
file(CONFIGURE
  OUTPUT ${HIPOBJ_SUBPROJECT_TEST_SETTINGS}
  CONTENT [[
set(TEST_DIR "@CMAKE_CURRENT_BINARY_DIR@/subproject-test")
set(PARENT_SOURCE_DIR "@CMAKE_CURRENT_SOURCE_DIR@/parent")
set(PARENT_CACHE "@HIPOBJ_SUBPROJECT_CACHE@")
set(GENERATOR "@CMAKE_GENERATOR@")
set(MAKE_PROGRAM "@CMAKE_MAKE_PROGRAM@")
]]
  @ONLY)

add_test(
  NAME hipobj-subproject
  COMMAND ${CMAKE_COMMAND}
    -DSETTINGS=${HIPOBJ_SUBPROJECT_TEST_SETTINGS}
    -DCONFIG=$<CONFIG>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run-subproject-test.cmake)
