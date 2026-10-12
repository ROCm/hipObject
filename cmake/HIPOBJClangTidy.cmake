# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional clang-tidy integration

include_guard(GLOBAL)

option(HIPOBJ_USE_CLANG_TIDY
  "Run clang-tidy during compilation" OFF)

if(HIPOBJ_USE_CLANG_TIDY)
  # clang-tidy parses each file with the compiler's flags, and rejects
  # the GCC-only ones
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "HIPOBJ_USE_CLANG_TIDY requires Clang as the C++ compiler "
      "(CMAKE_CXX_COMPILER)")
  endif()

  # Prefer the clang-tidy on PATH, and fall back to ROCm's. CMake
  # searches CMAKE_PREFIX_PATH, as a CMake or environment variable,
  # before PATH, and it may have ROCm's LLVM in it, so it's skipped.
  find_program(CLANG_TIDY_EXE
    NAMES clang-tidy
    PATHS ${ROCM_PATH}/llvm/bin
    NO_CMAKE_PATH
    NO_CMAKE_ENVIRONMENT_PATH)

  if(CLANG_TIDY_EXE)
    message(STATUS
      "clang-tidy enabled: ${CLANG_TIDY_EXE}")
  else()
    message(WARNING
      "clang-tidy requested but not found")
  endif()
endif()

# Run clang-tidy on a target's C++ sources
#
# This is set per target instead of with CMAKE_CXX_CLANG_TIDY so
# that third-party code pulled in with FetchContent isn't checked.
function(hipobj_set_clang_tidy target)
  if(HIPOBJ_USE_CLANG_TIDY AND CLANG_TIDY_EXE)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--extra-arg=-Wno-unknown-warning-option")
  endif()
endfunction()
