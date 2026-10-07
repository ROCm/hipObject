# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install hipObject and build a consumer against the installed package
#
# Usage:
#   cmake -DSETTINGS=<settings.cmake> [-DCONFIG=<config>] -P run-install-test.cmake
#
# SETTINGS is written by CMakeLists.txt in this directory. CONFIG is the
# build configuration to install, for multi-config generators.
#
# Steps:
#   1. Install only the runtime and devel components (what the runtime
#      and development packages contain) into a scratch prefix
#   2. Configure, build, and run the consumer project in consumer/
#      against that prefix

if(NOT SETTINGS)
  message(FATAL_ERROR "run-install-test: SETTINGS is not set")
endif()
include(${SETTINGS})

set(prefix ${TEST_DIR}/prefix)
set(consumer_build_dir ${TEST_DIR}/consumer-build)

set(config_args)
if(CONFIG)
  set(config_args --config ${CONFIG})
endif()

# Run a command and fail with its output if it fails
function(run_step description)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "run-install-test: ${description} failed:\n${output}")
  endif()
  message(STATUS "run-install-test: ${description}: OK")
endfunction()

file(REMOVE_RECURSE ${TEST_DIR})
file(MAKE_DIRECTORY ${TEST_DIR})

foreach(component IN ITEMS runtime devel)
  run_step("install the ${component} component"
    ${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${prefix}
      --component ${component} ${config_args})
endforeach()

# Pass the consumer's settings in an initial cache file, since some of
# them are lists and can't be passed on the command line here
list(PREPEND CONSUMER_PREFIX_PATH ${prefix})
file(WRITE ${TEST_DIR}/consumer-cache.cmake
  "set(CMAKE_C_COMPILER \"${C_COMPILER}\" CACHE FILEPATH \"\")\n"
  "set(CMAKE_C_FLAGS \"${CONSUMER_FLAGS}\" CACHE STRING \"\")\n"
  "set(CMAKE_EXE_LINKER_FLAGS \"${CONSUMER_LINK_FLAGS}\" CACHE STRING \"\")\n"
  "set(CMAKE_PREFIX_PATH \"${CONSUMER_PREFIX_PATH}\" CACHE STRING \"\")\n"
  "set(HIPOBJ_EXPECTED_VERSION \"${EXPECTED_VERSION}\" CACHE STRING \"\")\n")

set(generator_args -G ${GENERATOR})
if(MAKE_PROGRAM)
  list(APPEND generator_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()

run_step("configure the consumer"
  ${CMAKE_COMMAND} -S ${CONSUMER_SOURCE_DIR} -B ${consumer_build_dir}
    ${generator_args} -C ${TEST_DIR}/consumer-cache.cmake)

run_step("build the consumer"
  ${CMAKE_COMMAND} --build ${consumer_build_dir} ${config_args})

set(ctest_config_args)
if(CONFIG)
  set(ctest_config_args -C ${CONFIG})
endif()
run_step("run the consumer"
  ${CMAKE_CTEST_COMMAND} --test-dir ${consumer_build_dir}
    --output-on-failure ${ctest_config_args})
