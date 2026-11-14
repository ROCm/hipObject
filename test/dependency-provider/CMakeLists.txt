# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Configure hipObject with a dependency provider like the one TheRock
# sets in the projects it builds (see provider.cmake and
# run-dependency-provider-test.cmake). A dependency provider is a
# command that find_package() calls first. TheRock's finds each package
# that TheRock builds in TheRock's directory for it, and nowhere else,
# and fails a find_package() for one that the project didn't declare
# as a dependency. This catches a find_package() call that wouldn't
# work in TheRock, which nothing else here does.
#
# The test only configures hipObject. The hipobj-subproject test
# builds it.

# The packages TheRock builds that hipObject's configure can find, and
# the TheRock projects that provide them, which hipObject will have to
# declare as dependencies:
#   hip                    hip-clr
#   hsa-runtime64, hsakmt  ROCR-Runtime
#   AMDDeviceLibs          amd-llvm
#   amd_comgr              amd-comgr
#   ROCmCMakeBuildTools    rocm-cmake
#   GTest                  therock-googletest
# hip's package files find AMDDeviceLibs, amd_comgr, and hsa-runtime64,
# and hsa-runtime64's find hsakmt when ROCr is a static library.
set(therock_packages
  hip
  hsa-runtime64
  hsakmt
  AMDDeviceLibs
  amd_comgr
  ROCmCMakeBuildTools
  GTest)

# The packages that come from the system, including the ones that
# CMake's find modules look for (FindCURL looks for PkgConfig). The
# test's hipObject doesn't build the MinIO bridge or the documentation,
# which TheRock won't either, so their packages aren't here.
set(system_packages
  Threads
  CURL
  PkgConfig
  LibElf)

# The test provides a TheRock package from the directory where this
# build found it. A TheRock package this build didn't find, such as
# hsakmt with a shared ROCr, isn't provided, so a find_package() for
# it fails, as it would in TheRock. GoogleTest is the exception: when
# this build downloaded it, or found it without its package files, the
# test's hipObject looks for it as a system package, with this build's
# copy or the paths FindGTest found (see below), so it doesn't download
# it again.
if(NOT GTest_DIR)
  list(REMOVE_ITEM therock_packages GTest)
  list(APPEND system_packages GTest)
endif()

set(HIPOBJ_PROVIDER_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/dependency-provider-test)
set(HIPOBJ_PROVIDER_LOG ${HIPOBJ_PROVIDER_TEST_DIR}/provider.log)
set(HIPOBJ_PROVIDER_CACHE ${CMAKE_CURRENT_BINARY_DIR}/provider-cache.cmake)
string(CONCAT HIPOBJ_PROVIDER_CACHE_ENTRIES
  "set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES \"${CMAKE_CURRENT_SOURCE_DIR}/provider.cmake\" CACHE FILEPATH \"\")\n"
  "set(HIPOBJ_TEST_PROVIDER_LOG \"${HIPOBJ_PROVIDER_LOG}\" CACHE FILEPATH \"\")\n"
  "set(HIPOBJ_TEST_THEROCK_PACKAGES \"${therock_packages}\" CACHE STRING \"\")\n"
  "set(HIPOBJ_TEST_SYSTEM_PACKAGES \"${system_packages}\" CACHE STRING \"\")\n"
  "set(BUILD_TESTING ON CACHE BOOL \"\")\n")
set(provided_packages)
foreach(package IN LISTS therock_packages)
  set(package_dir "${${package}_DIR}")
  if(package_dir)
    list(APPEND provided_packages ${package})
    string(APPEND HIPOBJ_PROVIDER_CACHE_ENTRIES
      "set(HIPOBJ_TEST_PACKAGE_DIR_${package} \"${package_dir}\" CACHE PATH \"\")\n")
  endif()
endforeach()
string(APPEND HIPOBJ_PROVIDER_CACHE_ENTRIES
  "set(HIPOBJ_TEST_PROVIDED_PACKAGES \"${provided_packages}\" CACHE STRING \"\")\n")
if(googletest_SOURCE_DIR)
  string(APPEND HIPOBJ_PROVIDER_CACHE_ENTRIES
    "set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST \"${googletest_SOURCE_DIR}\" CACHE PATH \"\")\n")
elseif(NOT GTest_DIR)
  # FindGTest found GoogleTest without a config package, possibly
  # through a GTEST_ROOT hint, so pass on the paths it found
  foreach(var IN ITEMS
      GTEST_INCLUDE_DIR
      GTEST_LIBRARY
      GTEST_LIBRARY_DEBUG
      GTEST_MAIN_LIBRARY
      GTEST_MAIN_LIBRARY_DEBUG)
    if(${var})
      get_property(type CACHE ${var} PROPERTY TYPE)
      string(APPEND HIPOBJ_PROVIDER_CACHE_ENTRIES
        "set(${var} \"${${var}}\" CACHE ${type} \"\")\n")
    endif()
  endforeach()
endif()
foreach(var IN ITEMS
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    ROCM_PATH
    ROCM_VERSION
    BUILD_SHARED_LIBS
    HIPOBJ_CXX_STANDARD
    HIPOBJ_BNXT
    HIPOBJ_IONIC)
  string(APPEND HIPOBJ_PROVIDER_CACHE_ENTRIES
    "set(${var} \"${${var}}\" CACHE STRING \"\")\n")
endforeach()
file(WRITE ${HIPOBJ_PROVIDER_CACHE} "${HIPOBJ_PROVIDER_CACHE_ENTRIES}")

set(HIPOBJ_PROVIDER_TEST_SETTINGS
  ${CMAKE_CURRENT_BINARY_DIR}/dependency-provider-test-settings.cmake)
file(CONFIGURE
  OUTPUT ${HIPOBJ_PROVIDER_TEST_SETTINGS}
  CONTENT [[
set(TEST_DIR "@HIPOBJ_PROVIDER_TEST_DIR@")
set(HIPOBJ_SOURCE_DIR "@PROJECT_SOURCE_DIR@")
set(PROVIDER_CACHE "@HIPOBJ_PROVIDER_CACHE@")
set(PROVIDER_LOG "@HIPOBJ_PROVIDER_LOG@")
set(GENERATOR "@CMAKE_GENERATOR@")
set(MAKE_PROGRAM "@CMAKE_MAKE_PROGRAM@")
]]
  @ONLY)

add_test(
  NAME hipobj-dependency-provider
  COMMAND ${CMAKE_COMMAND}
    -DSETTINGS=${HIPOBJ_PROVIDER_TEST_SETTINGS}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run-dependency-provider-test.cmake)
