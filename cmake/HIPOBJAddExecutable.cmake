# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

include(HIPOBJClangTidy)
include(HIPOBJCompilerOptions)

# Add an executable program using hipObject build conventions
#
# Parameters:
#   NAME <name>                     The name of the executable program to create
#   SRCS [src1 [src2 ...]]          The source files
#
# NOTE: Include directories, link libraries, and compile definitions
#       are still set by the caller.
function(hipobj_add_executable)

  # Parse arguments
  set(options) # None at this time
  set(oneValueArgs NAME)
  set(multiValueArgs SRCS)
  cmake_parse_arguments(PARSE_ARGV 0 arg
    "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(NOT arg_NAME)
    message(FATAL_ERROR "hipobj_add_executable: NAME is required")
  endif()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "hipobj_add_executable: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()

  add_executable(${arg_NAME} ${arg_SRCS})

  # Set the C++ standard and position-independent code
  set_target_properties(${arg_NAME} PROPERTIES
    CXX_STANDARD ${HIPOBJ_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

  hipobj_set_compiler_flags(${arg_NAME})
  hipobj_set_clang_tidy(${arg_NAME})
endfunction()

# Add an executable test program using hipObject build conventions
#
# Parameters: Same as hipobj_add_executable()
#
# NOTE: Currently a pass-through to hipobj_add_executable(). Settings
#       that only apply to test programs go here.
function(hipobj_add_test_executable)
  hipobj_add_executable(${ARGN})
endfunction()
