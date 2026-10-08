# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional include-what-you-use (IWYU) integration
#
# When this option is enabled, compilation will emit IWYU suggestions.
# They are advisory: IWYU doesn't fail the build.

include_guard(GLOBAL)

option(HIPOBJ_USE_IWYU
  "Run include-what-you-use when compiling" OFF)

if(HIPOBJ_USE_IWYU)
  find_program(HIPOBJ_IWYU_EXE NAMES include-what-you-use REQUIRED)
  message(STATUS "include-what-you-use enabled: ${HIPOBJ_IWYU_EXE}")

  # GoogleTest/GoogleMock expose their public API through umbrella headers
  # (gtest/gtest.h, gmock/gmock.h) but define symbols in private sub-headers.
  # Without a mapping, IWYU suggests including those private headers and
  # confuses the quoted vs. angle-bracket spellings. This mapping file
  # redirects all gtest/gmock private headers to the public umbrella headers.
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
