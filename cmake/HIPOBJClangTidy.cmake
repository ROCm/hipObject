# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional clang-tidy integration

include_guard(GLOBAL)

option(HIPOBJ_USE_CLANG_TIDY
  "Run clang-tidy during compilation" OFF)

# .clang-tidy uses ExcludeHeaderFilterRegex, which clang-tidy 19 added
set(HIPOBJ_CLANG_TIDY_MIN_VERSION 19)

# Get the LLVM major version of a clang-tidy, or an empty string if it
# can't be run or its version can't be read
function(hipobj_get_clang_tidy_version outvar exe)
  execute_process(
    COMMAND "${exe}" --version
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_QUIET)
  set(version "")
  if(result EQUAL 0 AND output MATCHES "LLVM version ([0-9]+)")
    set(version "${CMAKE_MATCH_1}")
  endif()
  set(${outvar} "${version}" PARENT_SCOPE)
endfunction()

# find_program() validator that rejects a clang-tidy that's too old
function(hipobj_validate_clang_tidy result_var exe)
  hipobj_get_clang_tidy_version(version "${exe}")
  if(NOT version OR version VERSION_LESS HIPOBJ_CLANG_TIDY_MIN_VERSION)
    set(${result_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

if(HIPOBJ_USE_CLANG_TIDY)
  # clang-tidy parses each file with the compiler's flags, and rejects
  # the GCC-only ones
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "HIPOBJ_USE_CLANG_TIDY requires Clang as the C++ compiler "
      "(CMAKE_CXX_COMPILER)")
  endif()

  # Look on PATH for clang-tidy, then for the versioned names that
  # Linux distributions and apt.llvm.org use, newest first. CMake
  # searches CMAKE_PREFIX_PATH, as a CMake or environment variable,
  # before PATH, and it may have ROCm's LLVM in it, so it's skipped.
  set(versioned_names "")
  foreach(version RANGE ${HIPOBJ_CLANG_TIDY_MIN_VERSION} 30)
    list(PREPEND versioned_names clang-tidy-${version})
  endforeach()
  find_program(HIPOBJ_CLANG_TIDY_EXE
    NAMES clang-tidy ${versioned_names}
    NO_CMAKE_PATH
    NO_CMAKE_ENVIRONMENT_PATH
    VALIDATOR hipobj_validate_clang_tidy
    DOC "clang-tidy ${HIPOBJ_CLANG_TIDY_MIN_VERSION} or later")
  unset(versioned_names)

  # Fall back to ROCm's, which is built without the clang-analyzer-*
  # checks. find_program() doesn't search again if the first one found
  # a clang-tidy.
  find_program(HIPOBJ_CLANG_TIDY_EXE
    NAMES clang-tidy
    PATHS "${ROCM_PATH}/llvm/bin"
    NO_DEFAULT_PATH
    VALIDATOR hipobj_validate_clang_tidy
    DOC "clang-tidy ${HIPOBJ_CLANG_TIDY_MIN_VERSION} or later")

  if(NOT HIPOBJ_CLANG_TIDY_EXE)
    message(FATAL_ERROR
      "HIPOBJ_USE_CLANG_TIDY needs clang-tidy "
      "${HIPOBJ_CLANG_TIDY_MIN_VERSION} or later, which wasn't found. "
      "Set HIPOBJ_CLANG_TIDY_EXE to its path.")
  endif()

  # find_program() doesn't validate a path that was set, or found by an
  # earlier configuration, so check it here
  hipobj_get_clang_tidy_version(HIPOBJ_CLANG_TIDY_VERSION
    "${HIPOBJ_CLANG_TIDY_EXE}")
  if(NOT HIPOBJ_CLANG_TIDY_VERSION)
    message(FATAL_ERROR
      "Can't get the version of clang-tidy from "
      "HIPOBJ_CLANG_TIDY_EXE (${HIPOBJ_CLANG_TIDY_EXE})")
  elseif(HIPOBJ_CLANG_TIDY_VERSION VERSION_LESS HIPOBJ_CLANG_TIDY_MIN_VERSION)
    message(FATAL_ERROR
      "HIPOBJ_CLANG_TIDY_EXE (${HIPOBJ_CLANG_TIDY_EXE}) is clang-tidy "
      "${HIPOBJ_CLANG_TIDY_VERSION}, but hipObject needs "
      "${HIPOBJ_CLANG_TIDY_MIN_VERSION} or later")
  endif()

  message(STATUS
    "clang-tidy ${HIPOBJ_CLANG_TIDY_VERSION} enabled: "
    "${HIPOBJ_CLANG_TIDY_EXE}")
endif()

# Run clang-tidy on a target's C++ sources
#
# This is set per target instead of with CMAKE_CXX_CLANG_TIDY so
# that third-party code pulled in with FetchContent isn't checked.
function(hipobj_set_clang_tidy target)
  if(HIPOBJ_USE_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY
      "${HIPOBJ_CLANG_TIDY_EXE};--extra-arg=-Wno-unknown-warning-option")
  endif()
endfunction()
