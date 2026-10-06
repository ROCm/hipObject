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
include(HIPOBJSanitizers)

check_linker_flag(CXX "-Wl,-z,noexecstack"
  HIPOBJ_LINKER_SUPPORTS_NOEXECSTACK)

function(hipobj_set_compiler_flags target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter)

  if(HIPOBJ_USE_SANITIZERS)
    hipobj_add_sanitizers(${target})
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
endfunction()
