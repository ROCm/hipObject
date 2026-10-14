# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build hipObject as a subproject of another CMake project (see
# run-subproject-test.cmake). This catches CMake code that assumes
# hipObject is the top-level project, which nothing else here does.

# The parent project builds hipObject with this build's compilers,
# options, and dependencies. CMAKE_PREFIX_PATH is the value hipObject
# received (see the top-level CMakeLists.txt), so it includes any
# prefixes a project above hipObject added as a normal variable, but
# not the ROCm prefixes hipObject adds itself, which the nested
# hipObject adds again.
#
# The nested hipObject's tests are configured, so that the parent can
# check what their copy of this file passes on, but only the library is
# built, since building the tests would make the test take much longer.
# They use the GoogleTest this build found or fetched, so the test
# doesn't download it again.
set(HIPOBJ_SUBPROJECT_CACHE ${CMAKE_CURRENT_BINARY_DIR}/parent-cache.cmake)
string(CONCAT HIPOBJ_SUBPROJECT_CACHE_ENTRIES
  "set(HIPOBJ_SUBPROJECT_SOURCE_DIR \"${PROJECT_SOURCE_DIR}\" CACHE PATH \"\")\n"
  "set(CMAKE_PREFIX_PATH \"${HIPOBJ_INHERITED_PREFIX_PATH}\" CACHE STRING \"\")\n"
  "set(BUILD_TESTING ON CACHE BOOL \"\")\n"
  "set(HIPOBJ_INSTALL_EXAMPLES OFF CACHE BOOL \"\")\n")
if(googletest_SOURCE_DIR)
  string(APPEND HIPOBJ_SUBPROJECT_CACHE_ENTRIES
    "set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST \"${googletest_SOURCE_DIR}\" CACHE PATH \"\")\n")
elseif(GTest_DIR)
  string(APPEND HIPOBJ_SUBPROJECT_CACHE_ENTRIES
    "set(GTest_DIR \"${GTest_DIR}\" CACHE PATH \"\")\n")
endif()
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
