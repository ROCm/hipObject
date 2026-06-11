# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

if(NOT TARGET unofficial::curlpp::curlpp)
  include(${CMAKE_CURRENT_LIST_DIR}/../HIPOBJMinioCppDeps.cmake)
endif()

set(unofficial-curlpp_FOUND TRUE)
