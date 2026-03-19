# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT

function(hipobj_add_example name)
  add_executable(${name} ${ARGN})
  set_source_files_properties(
    ${ARGN} PROPERTIES LANGUAGE HIP)
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/rdma
    ${PROJECT_SOURCE_DIR}/src/s3)
  target_link_libraries(${name} PRIVATE
    hipobj hip::host dl)
  install(TARGETS ${name}
    RUNTIME DESTINATION bin/hipobj-examples)
endfunction()

hipobj_add_example(get-object get-object.cpp)
hipobj_add_example(put-object put-object.cpp)
