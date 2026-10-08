# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Check that every function in the public header is documented as
# serialized or concurrent, and that the documentation matches the code
# (see check_thread_safety_tags.py), and test the checker itself. Both
# read source files, so they don't depend on the build.

find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(
    NAME hipobj-thread-safety-tags
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/check_thread_safety_tags.py
      ${PROJECT_SOURCE_DIR}/include/hipobj.h
      ${PROJECT_SOURCE_DIR}/src/hipobj.cpp)

  # -B keeps Python from writing a __pycache__ directory into the source
  # tree when the test imports the checker
  add_test(
    NAME hipobj-thread-safety-tags-selftest
    COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/test_check_thread_safety_tags.py)
else()
  message(STATUS
    "Python 3.10 or later wasn't found, so the "
    "hipobj-thread-safety-tags tests aren't registered")
endif()
