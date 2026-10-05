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

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(STATUS
    "GTest not found, fetching from GitHub")
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY
      https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON
    CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON
    CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # GoogleTest runs in the same process as the tests, so
  # instrument it when we build it from source.
  if(HIPOBJ_USE_SANITIZERS)
    foreach(gtest_target gtest gtest_main gmock gmock_main)
      hipobj_add_sanitizers(${gtest_target})
    endforeach()
  endif()
endif()

function(hipobj_add_test TEST_NAME TEST_SOURCE)
  hipobj_add_test_executable(
    NAME ${TEST_NAME}
    SRCS ${TEST_SOURCE})
  # Link the unit-test object build (HIPOBJ_UNIT_TESTS), not the
  # shipped library, so tests can swap seam tables. The macro must
  # also be set on the test TU so headers declare the accessors.
  target_link_libraries(${TEST_NAME} PRIVATE
    hipobj_test_objects
    GTest::gtest
    GTest::gtest_main
  )
  if(TARGET hipobj_v2_server_objects)
    target_link_libraries(${TEST_NAME} PRIVATE hipobj_v2_server_objects)
  endif()
  target_compile_definitions(${TEST_NAME} PRIVATE HIPOBJ_UNIT_TESTS)
  if(HIPOBJECT_V2_API)
    target_compile_definitions(${TEST_NAME} PRIVATE HIPOBJECT_V2_API)
  endif()
  target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/rdma
    ${CMAKE_SOURCE_DIR}/src/s3
    ${CMAKE_SOURCE_DIR}/test/integration/rdma-test-server
  )
  add_test(NAME ${TEST_NAME}
    COMMAND ${TEST_NAME})
endfunction()
