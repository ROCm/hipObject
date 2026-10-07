# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

add_subdirectory(unit)
add_subdirectory(exports)
add_subdirectory(install)

if(BUILD_TESTING)
  add_subdirectory(integration)
endif()
