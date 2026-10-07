# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

hipobj_add_test_executable(
  NAME hipobj-cuobject-probe
  SRCS main.cpp)

target_link_libraries(hipobj-cuobject-probe PRIVATE
  cuobjserver::cuobjserver)

# The tests component isn't packaged
install(TARGETS hipobj-cuobject-probe
  RUNTIME DESTINATION bin/hipobj-test
  COMPONENT tests)
