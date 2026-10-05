# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sanitizer support (ASan, UBSan, TSan)

include_guard(GLOBAL)

option(HIPOBJ_USE_SANITIZERS
  "Enable sanitizers (address, undefined, thread)"
  OFF)

if(HIPOBJ_USE_SANITIZERS)
  set(HIPOBJ_SANITIZER_TYPE "address"
    CACHE STRING "Sanitizer type")
  set_property(CACHE HIPOBJ_SANITIZER_TYPE
    PROPERTY STRINGS
    "address" "undefined" "thread")
endif()

# Add the HIPOBJ_SANITIZER_TYPE compile and link flags to a target
function(hipobj_add_sanitizers target)
  target_compile_options(${target} PRIVATE
    -fsanitize=${HIPOBJ_SANITIZER_TYPE}
    -fno-omit-frame-pointer)
  target_link_options(${target} PRIVATE
    -fsanitize=${HIPOBJ_SANITIZER_TYPE})
endfunction()
