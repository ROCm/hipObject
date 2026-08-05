# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

add_executable(hipobj-cuobject-probe main.cpp)

target_compile_features(hipobj-cuobject-probe PRIVATE cxx_std_17)

target_link_libraries(hipobj-cuobject-probe PRIVATE
  cuobjserver::cuobjserver)

install(TARGETS hipobj-cuobject-probe
  RUNTIME DESTINATION bin/hipobj-test)
