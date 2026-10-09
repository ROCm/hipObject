# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Compiler and linker flags for hipObject targets
#
# The flags are applied per target (PRIVATE) by the hipobj_add_*
# functions instead of with add_compile_options(), so they don't
# leak into third-party code pulled in with FetchContent.

include_guard(GLOBAL)

include(CheckLinkerFlag)
include(HIPOBJClangCompilerOptions)
include(HIPOBJCoverage)
include(HIPOBJGNUCompilerOptions)
include(HIPOBJSanitizers)

check_linker_flag(CXX "-Wl,-z,noexecstack"
  HIPOBJ_LINKER_SUPPORTS_NOEXECSTACK)
check_linker_flag(CXX "-Wl,-z,now"
  HIPOBJ_LINKER_SUPPORTS_NOW)

# Off by default so a downstream or superproject build is never broken
# by a warning from a compiler version we haven't seen. CI turns it on,
# so new warnings still fail the build there.
option(HIPOBJ_WERROR
  "Treat compiler warnings as errors in hipObject targets" OFF)

# Add the compiler-specific warning flags (and -Werror, sanitizer, and
# code coverage flags, if enabled) to a target
#
# NOTE: The warning flags are only applied to C++ sources since many
#       of them are invalid for C.
function(hipobj_set_compiler_flags target)
  set(flags)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    hipobj_get_gnu_warning_flags(flags ${CMAKE_CXX_COMPILER_VERSION})
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    hipobj_get_clang_warning_flags(flags ${CMAKE_CXX_COMPILER_VERSION})
  endif()
  target_compile_options(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${flags}>")

  if(HIPOBJ_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()

  if(HIPOBJ_USE_SANITIZERS)
    hipobj_add_sanitizers(${target})
  endif()

  if(HIPOBJ_USE_CODE_COVERAGE)
    hipobj_add_coverage(${target})
  endif()
endfunction()

# Add hardening link options
#
# NOTE: CMake ignores link options on static and object libraries,
#       so these only take effect on executables and shared libraries.
function(hipobj_set_linker_flags target)
  if(HIPOBJ_LINKER_SUPPORTS_NOEXECSTACK)
    target_link_options(${target} PRIVATE "-Wl,-z,noexecstack")
  endif()

  # Resolve every symbol at load time, so the linker's RELRO segment
  # can cover the whole GOT (full RELRO)
  if(HIPOBJ_LINKER_SUPPORTS_NOW)
    target_link_options(${target} PRIVATE "-Wl,-z,now")
  endif()
endfunction()
