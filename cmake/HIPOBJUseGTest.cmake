# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Fetch or find GTest for unit testing

# This module fetches GoogleTest and applies settings to its
# targets, so it must run exactly once.
include_guard(GLOBAL)

include(FetchContent)
include(HIPOBJAddExecutable)
include(HIPOBJSanitizers)

# GoogleTest runs in the same process as the tests, so a sanitizer
# build has to instrument it too, which means building it from source.
# An installed GoogleTest isn't instrumented.
hipobj_get_sanitizer_flags(hipobj_sanitizer_compile_options
  hipobj_sanitizer_link_options)
set(hipobj_fetch_gtest TRUE)
if(hipobj_sanitizer_compile_options)
  message(STATUS
    "Fetching GTest from GitHub, to build it with the sanitizers")
else()
  find_package(GTest QUIET)
  if(GTest_FOUND)
    set(hipobj_fetch_gtest FALSE)
  else()
    message(STATUS
      "GTest not found, fetching from GitHub")
  endif()
endif()

if(hipobj_fetch_gtest)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY
      https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON
    CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF
    CACHE BOOL "" FORCE)
  # GoogleTest's install rules have no component, so without this,
  # cmake --install would put GoogleTest in hipObject's prefix
  set(INSTALL_GTEST OFF
    CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

function(hipobj_add_test TEST_NAME TEST_SOURCE)
  hipobj_add_test_executable(
    NAME ${TEST_NAME}
    SRCS ${TEST_SOURCE})
  # Link the unit-test object build, not the shipped library, so tests
  # can call the library's internal functions and classes.
  target_link_libraries(${TEST_NAME} PRIVATE
    hipobj_test_objects
    GTest::gtest
    GTest::gtest_main
  )
  # So tests know which vendor backends hipobj_test_objects has
  if(HIPOBJ_BNXT)
    target_compile_definitions(${TEST_NAME} PRIVATE HIPOBJ_BNXT)
  endif()
  if(HIPOBJ_IONIC)
    target_compile_definitions(${TEST_NAME} PRIVATE HIPOBJ_IONIC)
  endif()
  target_include_directories(${TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/shared
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/rdma
    ${PROJECT_SOURCE_DIR}/src/s3
    ${PROJECT_SOURCE_DIR}/test/common
    ${PROJECT_SOURCE_DIR}/test/integration/rdma-test-server
  )
  add_test(NAME ${TEST_NAME}
    COMMAND ${TEST_NAME})
endfunction()
