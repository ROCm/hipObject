# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# ROCm path and version detection, HIP platform configuration, and
# rocm-cmake, which HIPOBJInstall.cmake uses to install and package
# hipObject

include(FetchContent)

if(NOT CMAKE_HIP_PLATFORM)
  set(CMAKE_HIP_PLATFORM "amd"
    CACHE STRING "HIP platform to build with")
endif()
set_property(CACHE CMAKE_HIP_PLATFORM
  PROPERTY STRINGS "amd")

if(NOT CMAKE_HIP_PLATFORM STREQUAL "amd")
  message(FATAL_ERROR
    "Invalid CMAKE_HIP_PLATFORM: "
    "'${CMAKE_HIP_PLATFORM}'. "
    "Allowed value is 'amd'.")
endif()

set(ENV{HIP_PLATFORM} ${CMAKE_HIP_PLATFORM})

# Split a ROCm version into its major and minor numbers
#
# Parameters:
#   version     A MAJOR.MINOR[.PATCH] ROCm version
#   major_var   Variable to set to the major number
#   minor_var   Variable to set to the minor number
function(hipobj_split_rocm_version version major_var minor_var)
  if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)(\\.[0-9]+)?$")
    message(FATAL_ERROR
      "ROCM_VERSION='${version}' does not match the expected "
      "MAJOR.MINOR[.PATCH] form (e.g., 7.13.0).")
  endif()
  set(${major_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${minor_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Set the ROCm version and path. Each is taken from, in order:
#   1. The CMake variable
#   2. The environment variable
#   3. A default
#
# The default ROCm path depends on the ROCm version, when that's known.
# The default ROCm version is read from the ROCm installation.
if(NOT DEFINED ROCM_VERSION AND DEFINED ENV{ROCM_VERSION})
  set(ROCM_VERSION "$ENV{ROCM_VERSION}")
endif()

if(DEFINED ENV{ROCM_PATH})
  set(ROCM_INITIAL_PATH "$ENV{ROCM_PATH}")
elseif(DEFINED ROCM_VERSION)
  # ROCm 7.11+ installs to /opt/rocm/core-MAJOR.MINOR (new repo.amd.com
  # layout); earlier releases install to /opt/rocm-MAJOR.MINOR.PATCH.
  hipobj_split_rocm_version("${ROCM_VERSION}"
    HIPOBJ_ROCM_VERSION_MAJOR HIPOBJ_ROCM_VERSION_MINOR)
  if(ROCM_VERSION VERSION_GREATER_EQUAL 7.11)
    set(ROCM_INITIAL_PATH
      "/opt/rocm/core-${HIPOBJ_ROCM_VERSION_MAJOR}.${HIPOBJ_ROCM_VERSION_MINOR}")
  else()
    set(ROCM_INITIAL_PATH "/opt/rocm-${ROCM_VERSION}")
  endif()
elseif(EXISTS "/opt/rocm/core/.info/version")
  set(ROCM_INITIAL_PATH "/opt/rocm/core")
else()
  set(ROCM_INITIAL_PATH "/opt/rocm")
endif()
set(ROCM_PATH "${ROCM_INITIAL_PATH}"
  CACHE PATH "The path to the ROCm installation")

if(NOT DEFINED ROCM_VERSION)
  # In the ROCm 7.11+ layout, /opt/rocm only holds links into the
  # installed versions, and the version file is in /opt/rocm/core,
  # which links to the selected version
  if(EXISTS "${ROCM_PATH}/.info/version")
    set(HIPOBJ_ROCM_VERSION_PATH "${ROCM_PATH}/.info/version")
  elseif(EXISTS "${ROCM_PATH}/core/.info/version")
    set(HIPOBJ_ROCM_VERSION_PATH "${ROCM_PATH}/core/.info/version")
  else()
    message(FATAL_ERROR
      "Can't determine the ROCm version: neither "
      "${ROCM_PATH}/.info/version nor ${ROCM_PATH}/core/.info/version "
      "exists. Set ROCM_VERSION.")
  endif()
  file(READ "${HIPOBJ_ROCM_VERSION_PATH}" HIPOBJ_ROCM_VERSION_FILE)
  string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+"
    ROCM_VERSION "${HIPOBJ_ROCM_VERSION_FILE}")
endif()
set(ROCM_VERSION "${ROCM_VERSION}"
  CACHE STRING "The version of ROCm to build with")

# HIPOBJInstall.cmake picks the package dependencies with these
hipobj_split_rocm_version("${ROCM_VERSION}"
  HIPOBJ_ROCM_VERSION_MAJOR HIPOBJ_ROCM_VERSION_MINOR)

message(STATUS "Using ROCM_VERSION: ${ROCM_VERSION}")
message(STATUS "ROCM_PATH set to: ${ROCM_PATH}")

# When hipObject is a subproject, the install prefix is the
# superproject's to choose
if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${ROCM_PATH}"
    CACHE PATH
    "The path where hipObject should be installed"
    FORCE)
endif()

set(CMAKE_INSTALL_LIBDIR "lib"
  CACHE STRING
  "Directory name for installed ROCm libraries")

list(APPEND CMAKE_PREFIX_PATH ${ROCM_PATH})

# Use the rocm-cmake that comes with ROCm, or fetch it if there isn't
# one. FetchContent_Declare's FIND_PACKAGE_ARGS would do this, but it
# needs CMake 3.24.
find_package(ROCmCMakeBuildTools CONFIG QUIET)
if(NOT ROCmCMakeBuildTools_FOUND)
  # rocm-cmake is only a collection of CMake modules, so it's
  # downloaded but not added to the build
  FetchContent_Declare(
    rocm-cmake
    GIT_REPOSITORY https://github.com/ROCm/rocm-cmake.git
    GIT_TAG ecc716b97c2239cff00422ed7a43cd52a0839a0e # rocm-6.4.3
    SOURCE_SUBDIR "DISABLE ADDING TO BUILD"
  )
  FetchContent_MakeAvailable(rocm-cmake)
  find_package(ROCmCMakeBuildTools CONFIG REQUIRED
    NO_DEFAULT_PATH PATHS "${rocm-cmake_SOURCE_DIR}")
  message(STATUS "Using fetched rocm-cmake")
else()
  message(STATUS
    "rocm-cmake found @ ${ROCmCMakeBuildTools_DIR}")
endif()
