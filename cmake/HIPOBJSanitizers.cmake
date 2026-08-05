# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sanitizer support (ASan, UBSan, TSan)

option(HIPOBJ_USE_SANITIZERS
  "Enable sanitizers (address, undefined, thread)"
  OFF)

if(HIPOBJ_USE_SANITIZERS)
  set(HIPOBJ_SANITIZER_TYPE "address"
    CACHE STRING "Sanitizer type")
  set_property(CACHE HIPOBJ_SANITIZER_TYPE
    PROPERTY STRINGS
    "address" "undefined" "thread")

  add_compile_options(
    -fsanitize=${HIPOBJ_SANITIZER_TYPE}
    -fno-omit-frame-pointer)
  add_link_options(
    -fsanitize=${HIPOBJ_SANITIZER_TYPE})
endif()
