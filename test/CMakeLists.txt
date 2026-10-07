# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

add_subdirectory(unit)
add_subdirectory(exports)
add_subdirectory(header)

# The install test runs cmake --install, which needs the install rules
if(NOT CMAKE_SKIP_INSTALL_RULES)
  add_subdirectory(install)
endif()

if(BUILD_TESTING)
  add_subdirectory(integration)
endif()
