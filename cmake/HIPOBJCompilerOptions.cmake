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
include(CheckPIESupported)
include(HIPOBJClangCompilerOptions)
include(HIPOBJCoverage)
include(HIPOBJGNUCompilerOptions)

# Linker hardening, following the OpenSSF Compiler Options Hardening Guide
# for C and C++. Some distributions' compilers pass some of these by
# default (Ubuntu's GCC passes all but --no-copy-dt-needed-entries, which
# GNU ld and lld already default to), but upstream Clang, which TheRock
# uses, doesn't, so they're passed explicitly.
#
# https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html
check_linker_flag(CXX "-Wl,-z,noexecstack"
  HIPOBJ_LINKER_SUPPORTS_NOEXECSTACK)
check_linker_flag(CXX "-Wl,-z,relro"
  HIPOBJ_LINKER_SUPPORTS_RELRO)
check_linker_flag(CXX "-Wl,-z,now"
  HIPOBJ_LINKER_SUPPORTS_NOW)
check_linker_flag(CXX "-Wl,--as-needed"
  HIPOBJ_LINKER_SUPPORTS_AS_NEEDED)
check_linker_flag(CXX "-Wl,--no-copy-dt-needed-entries"
  HIPOBJ_LINKER_SUPPORTS_NO_COPY_DT_NEEDED)

# Without this, POSITION_INDEPENDENT_CODE only adds -fPIE to the compile,
# not -pie to the link, and an executable is only a PIE if the compiler
# links one by default. Every hipObject executable is linked as C++.
check_pie_supported(OUTPUT_VARIABLE hipobj_pie_output LANGUAGES CXX)
if(NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
  message(WARNING
    "The C++ linker can't build position-independent executables, so "
    "hipObject's programs won't be:\n${hipobj_pie_output}")
endif()

# Off by default so a downstream or superproject build is never broken
# by a warning from a compiler version we haven't seen. CI turns it on,
# so new warnings still fail the build there.
option(HIPOBJ_WERROR
  "Treat compiler warnings as errors in hipObject targets" OFF)

# Add the compiler-specific warning flags (and -Werror and code coverage
# flags, if enabled) to a target
#
# NOTE: HIPOBJSanitizers.cmake adds the sanitizer flags to every target
#       itself.
#
# NOTE: The warning flags are only applied to C++ sources since many
#       of them are invalid for C.
function(hipobj_set_compiler_flags target)
  set(flags)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    hipobj_get_gnu_warning_flags(flags ${CMAKE_CXX_COMPILER_VERSION})
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # IWYU gets the same flags as the compiler, so in an IWYU build, use
    # only the flags that IWYU's clang has too (see HIPOBJIWYU.cmake)
    set(clang_version ${CMAKE_CXX_COMPILER_VERSION})
    if(HIPOBJ_USE_IWYU AND HIPOBJ_IWYU_CLANG_VERSION
        AND HIPOBJ_IWYU_CLANG_VERSION VERSION_LESS clang_version)
      set(clang_version ${HIPOBJ_IWYU_CLANG_VERSION})
    endif()
    hipobj_get_clang_warning_flags(flags ${clang_version})
  endif()
  target_compile_options(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${flags}>")

  if(HIPOBJ_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()

  if(HIPOBJ_USE_CODE_COVERAGE)
    hipobj_add_coverage(${target})
  endif()
endfunction()

# Add hardening link options
#
# NOTE: CMake ignores link options on static and object libraries,
#       so these only take effect on executables and shared libraries.
#
# NOTE: The target's POSITION_INDEPENDENT_CODE property, which the
#       hipobj_add_* functions set, makes an executable a PIE, now that
#       check_pie_supported() has run (above).
function(hipobj_set_linker_flags target)
  if(HIPOBJ_LINKER_SUPPORTS_NOEXECSTACK)
    target_link_options(${target} PRIVATE "-Wl,-z,noexecstack")
  endif()

  # Make the GOT and the other data that only the dynamic linker writes
  # read-only once it has finished relocating (RELRO), and resolve every
  # symbol at load time, so RELRO covers the whole GOT (full RELRO)
  if(HIPOBJ_LINKER_SUPPORTS_RELRO)
    target_link_options(${target} PRIVATE "-Wl,-z,relro")
  endif()
  if(HIPOBJ_LINKER_SUPPORTS_NOW)
    target_link_options(${target} PRIVATE "-Wl,-z,now")
  endif()

  # Only record the shared libraries the target uses directly, so it
  # doesn't load ones it never calls, and the packages don't depend on
  # them
  #
  # NOTE: --as-needed only affects the libraries after it on the link
  #       line. CMake puts link options before the objects and libraries,
  #       so it covers all of them.
  if(HIPOBJ_LINKER_SUPPORTS_AS_NEEDED)
    target_link_options(${target} PRIVATE "-Wl,--as-needed")
  endif()
  if(HIPOBJ_LINKER_SUPPORTS_NO_COPY_DT_NEEDED)
    target_link_options(${target} PRIVATE
      "-Wl,--no-copy-dt-needed-entries")
  endif()
endfunction()
