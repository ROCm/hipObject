# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Code coverage (LLVM source-based coverage, Clang only)
#
# With HIPOBJ_USE_CODE_COVERAGE on, hipObject's targets are instrumented.
# Each instrumented program writes its profile to coverage/profraw in the
# build directory, however it's run, and the hipobj-coverage target
# reports the coverage of the library sources (src/) from them:
#
#   ctest
#   cmake --build . --target hipobj-coverage
#
# The report goes in coverage/report.txt (a summary for each file) and
# coverage/lines.txt (the coverage of each line). The profiles add up
# over test runs; delete coverage/profraw to start over.

include_guard(GLOBAL)

option(HIPOBJ_USE_CODE_COVERAGE
  "Build with LLVM code coverage instrumentation (Clang only)" OFF)

set(HIPOBJ_COVERAGE_DIR "${PROJECT_BINARY_DIR}/coverage")

# Find the llvm-profdata and llvm-cov that match the compiler, since the
# profile format changes between LLVM versions. Look next to the compiler
# first, and for the versioned names that Linux distributions use.
function(hipobj_find_coverage_tools)
  string(REGEX MATCH "^[0-9]+" major "${CMAKE_CXX_COMPILER_VERSION}")
  get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(compiler_path "${CMAKE_CXX_COMPILER}" REALPATH)
  get_filename_component(compiler_real_dir "${compiler_path}" DIRECTORY)

  foreach(tool IN ITEMS profdata cov)
    string(TOUPPER ${tool} tool_upper)
    find_program(HIPOBJ_LLVM_${tool_upper}
      NAMES llvm-${tool}-${major} llvm-${tool}
      NAMES_PER_DIR
      HINTS "${compiler_dir}" "${compiler_real_dir}"
      DOC "llvm-${tool} for the code coverage report")
    if(NOT HIPOBJ_LLVM_${tool_upper})
      message(FATAL_ERROR
        "HIPOBJ_USE_CODE_COVERAGE needs llvm-${tool}, which wasn't found "
        "with ${CMAKE_CXX_COMPILER}. Set HIPOBJ_LLVM_${tool_upper} to its "
        "path.")
    endif()
  endforeach()
endfunction()

# Add the code coverage instrumentation to a target
#
# The profile's file name is built in, so programs that the tests run
# indirectly, or that load the shared library, record their coverage
# too. %m gives each executable and shared library a file of its own,
# which the runtime merges each run into, so concurrent runs don't
# collide. LLVM_PROFILE_FILE overrides the name.
function(hipobj_add_coverage target)
  target_compile_options(${target} PRIVATE
    "-fprofile-instr-generate=${HIPOBJ_COVERAGE_DIR}/profraw/%m.profraw"
    -fcoverage-mapping)
  target_link_options(${target} PRIVATE -fprofile-instr-generate)

  # The report reads the coverage mapping from the linked binaries.
  # Static and object libraries end up in executables, so they don't
  # need to be listed.
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY")
    set_property(GLOBAL APPEND PROPERTY
      HIPOBJ_COVERAGE_BINARIES "$<TARGET_FILE:${target}>")
  endif()
endfunction()

# Add the hipobj-coverage target, which merges the profiles and writes
# the report (see HIPOBJCoverageReport.cmake). It has to run once all
# the instrumented targets have been added. The binaries' generator
# expressions make the target depend on them, so it builds any that are
# out of date.
function(hipobj_add_coverage_target)
  get_property(binaries GLOBAL PROPERTY HIPOBJ_COVERAGE_BINARIES)
  add_custom_target(hipobj-coverage
    COMMAND ${CMAKE_COMMAND}
      "-DLLVM_PROFDATA=${HIPOBJ_LLVM_PROFDATA}"
      "-DLLVM_COV=${HIPOBJ_LLVM_COV}"
      "-DCOVERAGE_DIR=${HIPOBJ_COVERAGE_DIR}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src"
      "-DBINARIES=${binaries}"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/HIPOBJCoverageReport.cmake"
    COMMAND ${CMAKE_COMMAND} -E cat "${HIPOBJ_COVERAGE_DIR}/report.txt"
    COMMENT "Generating the code coverage report"
    VERBATIM)
endfunction()

if(HIPOBJ_USE_CODE_COVERAGE)
  # C programs that link the static library, such as the install test's
  # consumer, need Clang to link the profile runtime
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
      OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "HIPOBJ_USE_CODE_COVERAGE requires Clang as the C and C++ compiler "
      "(CMAKE_C_COMPILER and CMAKE_CXX_COMPILER)")
  endif()
  hipobj_find_coverage_tools()
  cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
    CALL hipobj_add_coverage_target)
endif()
