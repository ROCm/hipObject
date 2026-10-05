# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Compiler flags for hipObject targets
#
# The flags are applied per target (PRIVATE) by the hipobj_add_*
# functions instead of with add_compile_options(), so they don't
# leak into third-party code pulled in with FetchContent.

include_guard(GLOBAL)

include(HIPOBJSanitizers)

function(hipobj_set_compiler_flags target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter)

  if(HIPOBJ_USE_SANITIZERS)
    hipobj_add_sanitizers(${target})
  endif()
endfunction()
