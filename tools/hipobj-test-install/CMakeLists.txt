# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

hipobj_add_executable(
  NAME hipobj-test-install
  SRCS hipobj-test-install.cpp)

target_include_directories(hipobj-test-install PRIVATE
  ${PROJECT_SOURCE_DIR}/shared)

target_link_libraries(hipobj-test-install PRIVATE
  hipobj)

# A static build has no runtime package, so it installs the program but
# doesn't package it
hipobj_install_executable(
  NAME hipobj-test-install
  DESTINATION bin)
