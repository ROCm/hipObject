# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

include(HIPOBJClangTidy)
include(HIPOBJCompilerOptions)

# Add a library using hipObject build conventions
#
# Parameters:
#   NAME <name>                     The name of the library to create
#   TYPE STATIC|SHARED|OBJECT       The library type (optional, defaults
#                                   to whatever BUILD_SHARED_LIBS selects)
#   SRCS [src1 [src2 ...]]          The source files
#
# NOTE: Include directories, link libraries, and compile definitions
#       are still set by the caller.
function(hipobj_add_library)

  # Parse arguments
  set(options) # None at this time
  set(oneValueArgs NAME TYPE)
  set(multiValueArgs SRCS)
  cmake_parse_arguments(PARSE_ARGV 0 arg
    "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(NOT arg_NAME)
    message(FATAL_ERROR "hipobj_add_library: NAME is required")
  endif()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "hipobj_add_library: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()

  add_library(${arg_NAME} ${arg_TYPE} ${arg_SRCS})

  # Set the C++ standard and position-independent code
  set_target_properties(${arg_NAME} PROPERTIES
    CXX_STANDARD ${HIPOBJ_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

  hipobj_set_compiler_flags(${arg_NAME})
  hipobj_set_clang_tidy(${arg_NAME})
endfunction()
