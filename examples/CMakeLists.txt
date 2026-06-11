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

find_package(CURL QUIET)

if(CURL_FOUND)
  hipobj_add_example(get-object get-object.cpp s3_curl_ops.cpp)
  hipobj_add_example(put-object put-object.cpp s3_curl_ops.cpp)
  target_compile_definitions(get-object PRIVATE HIPOBJ_HAVE_CURL)
  target_compile_definitions(put-object PRIVATE HIPOBJ_HAVE_CURL)
  target_link_libraries(get-object PRIVATE CURL::libcurl)
  target_link_libraries(put-object PRIVATE CURL::libcurl)
  message(STATUS "hipObject examples: libcurl enabled (--live mode)")
else()
  hipobj_add_example(get-object get-object.cpp)
  hipobj_add_example(put-object put-object.cpp)
  message(STATUS "hipObject examples: libcurl not found (stub mode only)")
endif()
