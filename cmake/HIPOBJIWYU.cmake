# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional include-what-you-use (IWYU) integration
#
# When this option is enabled, compilation will emit IWYU suggestions.
# They are advisory: IWYU doesn't fail the build.
#
# A build with this option is for checking includes, not for building
# hipObject to use. IWYU gets the same flags as the compiler, and it
# parses with the clang it was built with, which rejects flags that are
# newer than that clang. So when the compiler is a newer Clang (such as
# amdclang), the build leaves out the flags that IWYU's clang doesn't
# have, including some hardening flags (see hipobj_set_compiler_flags()).

include_guard(GLOBAL)

option(HIPOBJ_USE_IWYU
  "Run include-what-you-use when compiling" OFF)

if(HIPOBJ_USE_IWYU)
  find_program(HIPOBJ_IWYU_EXE NAMES include-what-you-use REQUIRED)
  message(STATUS "include-what-you-use enabled: ${HIPOBJ_IWYU_EXE}")

  # The version of IWYU's clang, from a line like "include-what-you-use
  # 0.23 based on Ubuntu clang version 19.1.7 (20ubuntu4)". Without it,
  # the build keeps every flag, and IWYU may reject some of them.
  set(HIPOBJ_IWYU_CLANG_VERSION "")
  execute_process(
    COMMAND "${HIPOBJ_IWYU_EXE}" --version
    RESULT_VARIABLE iwyu_version_result
    OUTPUT_VARIABLE iwyu_version_output
    ERROR_VARIABLE iwyu_version_output)
  if(iwyu_version_result EQUAL 0
      AND iwyu_version_output MATCHES "clang version ([0-9]+(\\.[0-9]+)*)")
    set(HIPOBJ_IWYU_CLANG_VERSION "${CMAKE_MATCH_1}")
    message(STATUS "include-what-you-use's clang: ${HIPOBJ_IWYU_CLANG_VERSION}")
  else()
    message(WARNING
      "Can't get the version of include-what-you-use's clang, so the "
      "build may use flags it rejects:\n${iwyu_version_output}")
  endif()

  # GoogleTest exposes its public API through an umbrella header
  # (gtest/gtest.h) but defines symbols in private sub-headers. Without a
  # mapping, IWYU suggests including those private headers and confuses
  # the quoted vs. angle-bracket spellings. This mapping file redirects
  # all gtest private headers to the public umbrella header.
  # libstdc++/glibc declare some symbols in private bits/* headers that IWYU
  # would otherwise suggest including directly (e.g. <bits/chrono.h> for
  # std::chrono, <bits/statx-generic.h> for statx). These are not part of the
  # bundled IWYU mappings, so redirect them to their public headers.
  # HIP declares types such as hipMemcpyKind in hip/driver_types.h, which
  # only compiles after hip/hip_runtime_api.h (it uses hipMemoryType
  # from there), so redirect it to hip/hip_runtime_api.h.
  set(IWYU_MAPPINGS
    "${CMAKE_CURRENT_LIST_DIR}/iwyu-gtest.imp"
    "${CMAKE_CURRENT_LIST_DIR}/iwyu-hip.imp"
    "${CMAKE_CURRENT_LIST_DIR}/iwyu-libc.imp")

  # IWYU parses with its own clang, which doesn't know every warning
  # flag the build uses (e.g., the GCC-only ones), so don't warn about
  # those
  set(IWYU_COMMAND "${HIPOBJ_IWYU_EXE}" "-Wno-unknown-warning-option")
  foreach(mapping IN LISTS IWYU_MAPPINGS)
    if(NOT EXISTS "${mapping}")
      message(FATAL_ERROR "IWYU mapping file not found: ${mapping}")
    endif()
    list(APPEND IWYU_COMMAND "-Xiwyu" "--mapping_file=${mapping}")
  endforeach()
endif()

# Run include-what-you-use on a target's C and C++ sources
#
# This is set per target instead of with CMAKE_<LANG>_INCLUDE_WHAT_YOU_USE
# so that third-party code pulled in with FetchContent isn't checked.
function(hipobj_set_iwyu target)
  if(HIPOBJ_USE_IWYU)
    set_target_properties(${target} PROPERTIES
      C_INCLUDE_WHAT_YOU_USE "${IWYU_COMMAND}"
      CXX_INCLUDE_WHAT_YOU_USE "${IWYU_COMMAND}")
  endif()
endfunction()
