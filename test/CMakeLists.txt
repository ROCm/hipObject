# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

add_subdirectory(unit)
add_subdirectory(exports)

# The install and package tests need the install rules
if(NOT CMAKE_SKIP_INSTALL_RULES)
  add_subdirectory(install)
endif()

if(BUILD_TESTING)
  add_subdirectory(integration)
endif()
