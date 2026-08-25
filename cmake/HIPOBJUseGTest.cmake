# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Fetch or find GTest for unit testing

include(FetchContent)

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
endif()

function(hipobj_add_test TEST_NAME TEST_SOURCE)
  add_executable(${TEST_NAME} ${TEST_SOURCE})
  # Link the unit-test object build (HIPOBJ_UNIT_TESTS), not the
  # shipped library, so tests can swap seam tables. The macro must
  # also be set on the test TU so headers declare the accessors.
  target_link_libraries(${TEST_NAME} PRIVATE
    hipobj_test_objects
    GTest::gtest
    GTest::gtest_main
  )
  target_compile_definitions(${TEST_NAME} PRIVATE HIPOBJ_UNIT_TESTS)
  target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shared
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/rdma
    ${CMAKE_SOURCE_DIR}/src/s3
  )
  add_test(NAME ${TEST_NAME}
    COMMAND ${TEST_NAME})
endfunction()
