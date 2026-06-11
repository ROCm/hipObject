# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

if(NOT TARGET unofficial::inih::inireader)
  include(${CMAKE_CURRENT_LIST_DIR}/../HIPOBJMinioCppDeps.cmake)
endif()

set(unofficial-inih_FOUND TRUE)
