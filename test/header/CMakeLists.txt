# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Compile hipobj.h as C11 and as C++11 with strict warnings, so a
# construct in the header that isn't valid in both languages fails the
# build. These are the oldest standards the header supports. The
# objects are never linked.
#
# The include directory isn't SYSTEM, since that would suppress
# warnings from the header. HIPOBJECT_V2_API is defined whatever the
# option is set to, so the whole header is checked.

add_library(hipobj_header_check OBJECT
  header-check.c
  header-check.cpp)

set_target_properties(hipobj_header_check PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

target_include_directories(hipobj_header_check
  PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_compile_definitions(hipobj_header_check
  PRIVATE HIPOBJECT_V2_API)

target_compile_options(hipobj_header_check
  PRIVATE -Wall -Wextra -Werror -pedantic)
