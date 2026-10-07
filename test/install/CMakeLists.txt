# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install hipObject into a scratch prefix and build a C-only consumer
# against the installed package (see run-install-test.cmake). This
# catches problems with the exported targets, package config, install
# components, and run paths, none of which the in-tree tests use.

# The consumer must use the same sanitizer as an instrumented libhipobj,
# or it can't link against the sanitizer runtime the library needs.
# Clang only links a sanitizer's C++ runtime (e.g., UBSan's vptr checks)
# when the C++ driver links, so the C consumer has to ask for it, along
# with the libstdc++ that runtime depends on.
set(HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS "")
set(HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS "")
if(HIPOBJ_USE_SANITIZERS)
  set(HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS
    "-fsanitize=${HIPOBJ_SANITIZER_TYPE}")
  set(HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS
    "${HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS}")
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    string(APPEND HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS
      " -fsanitize-link-c++-runtime -lstdc++")
  endif()
endif()

# The consumer finds hip and hsa-runtime64 (which the static library's
# package config looks for) where this build found them
set(HIPOBJ_INSTALL_TEST_PREFIX_PATH ${CMAKE_PREFIX_PATH})
foreach(package IN ITEMS hip hsa-runtime64)
  if(${package}_DIR)
    # <prefix>/lib/cmake/<package>
    get_filename_component(package_prefix "${${package}_DIR}/../../.."
      ABSOLUTE)
    list(APPEND HIPOBJ_INSTALL_TEST_PREFIX_PATH ${package_prefix})
  endif()
endforeach()
list(REMOVE_DUPLICATES HIPOBJ_INSTALL_TEST_PREFIX_PATH)

set(HIPOBJ_INSTALL_TEST_SETTINGS
  ${CMAKE_CURRENT_BINARY_DIR}/install-test-settings.cmake)
file(CONFIGURE
  OUTPUT ${HIPOBJ_INSTALL_TEST_SETTINGS}
  CONTENT [[
set(BUILD_DIR "@CMAKE_BINARY_DIR@")
set(TEST_DIR "@CMAKE_CURRENT_BINARY_DIR@/install-test")
set(CONSUMER_SOURCE_DIR "@CMAKE_CURRENT_SOURCE_DIR@/consumer")
set(GENERATOR "@CMAKE_GENERATOR@")
set(MAKE_PROGRAM "@CMAKE_MAKE_PROGRAM@")
set(C_COMPILER "@CMAKE_C_COMPILER@")
set(CONSUMER_FLAGS "@HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS@")
set(CONSUMER_LINK_FLAGS "@HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS@")
set(CONSUMER_PREFIX_PATH "@HIPOBJ_INSTALL_TEST_PREFIX_PATH@")
set(EXPECTED_VERSION "@HIPOBJ_LIBRARY_VERSION@")
]]
  @ONLY)

add_test(
  NAME hipobj-install
  COMMAND ${CMAKE_COMMAND}
    -DSETTINGS=${HIPOBJ_INSTALL_TEST_SETTINGS}
    -DCONFIG=$<CONFIG>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run-install-test.cmake)
