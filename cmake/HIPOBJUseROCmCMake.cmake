# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# ROCm path detection and HIP platform configuration

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

if(DEFINED ENV{ROCM_PATH})
  set(ROCM_INITIAL_PATH "$ENV{ROCM_PATH}")
else()
  set(ROCM_INITIAL_PATH "/opt/rocm")
endif()
set(ROCM_PATH "${ROCM_INITIAL_PATH}"
  CACHE PATH "The path to the ROCm installation")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${ROCM_PATH}"
    CACHE PATH
    "The path where hipObject should be installed"
    FORCE)
endif()

set(CMAKE_INSTALL_LIBDIR "lib"
  CACHE STRING
  "Directory name for installed ROCm libraries")

list(APPEND CMAKE_PREFIX_PATH ${ROCM_PATH})
