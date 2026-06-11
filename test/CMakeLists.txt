# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT

add_subdirectory(unit)

if(BUILD_TESTING)
  add_subdirectory(integration)
endif()
