# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Optional download of NVIDIA cuObject libraries.
#
# cuObject Client: publicly available on the
#   NVIDIA CUDA redistributable CDN.
# cuObject Server: NOT on the public CDN; must
#   be installed separately (e.g. from NVIDIA's
#   partner channel or a .deb package).
#
# Options:
#   HIPOBJ_FETCH_CUOBJECT_CLIENT  (default OFF)
#     Downloads libcuobjclient from NVIDIA CDN
#     and creates an IMPORTED target.
#
#   HIPOBJ_FIND_CUOBJECT_SERVER   (default OFF)
#     Searches for a system-installed
#     libcuobjserver. Set CUOBJSERVER_ROOT to
#     guide the search.
#
# Targets created:
#   cuobjclient::cuobjclient  (if client found)
#   cuobjserver::cuobjserver  (if server found)
#
# Variables exported:
#   CUOBJECT_CLIENT_FOUND
#   CUOBJECT_CLIENT_INCLUDE_DIR
#   CUOBJECT_CLIENT_LIBRARY
#   CUOBJECT_SERVER_FOUND
#   CUOBJECT_SERVER_INCLUDE_DIR
#   CUOBJECT_SERVER_LIBRARY

include(FetchContent)

set(CUOBJECT_CLIENT_VERSION "1.2.0.59"
  CACHE STRING
  "cuObject client library version to fetch")

set(CUOBJSERVER_ROOT "" CACHE PATH
  "Path to cuObject server installation")

# ---- cuObject Client (CDN download) --------

option(HIPOBJ_FETCH_CUOBJECT_CLIENT
  "Download NVIDIA cuObject client library" OFF)

if(HIPOBJ_FETCH_CUOBJECT_CLIENT)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES
      "aarch64|arm64")
    set(_cuobj_platform "linux-sbsa")
    set(_cuobj_sha256
      "b78b3e704303f56dfddc9411a43f40adad3951b301180549fe7d9b7809905efb")
  else()
    set(_cuobj_platform "linux-x86_64")
    set(_cuobj_sha256
      "72a07d05c79bedc8b20a6a41e7ecf9ece63908a6ae946219110a6688db9b82a0")
  endif()

  set(_cuobj_base
    "https://developer.download.nvidia.com")
  set(_cuobj_path
    "compute/cuda/redist/libcuobjclient")
  set(_cuobj_archive
    "libcuobjclient-${_cuobj_platform}")
  set(_cuobj_archive
    "${_cuobj_archive}-${CUOBJECT_CLIENT_VERSION}")
  set(_cuobj_archive
    "${_cuobj_archive}-archive.tar.xz")
  set(_cuobj_url
    "${_cuobj_base}/${_cuobj_path}")
  set(_cuobj_url
    "${_cuobj_url}/${_cuobj_platform}")
  set(_cuobj_url
    "${_cuobj_url}/${_cuobj_archive}")

  message(STATUS
    "Fetching cuObject client "
    "${CUOBJECT_CLIENT_VERSION} from NVIDIA CDN")

  FetchContent_Declare(cuobjclient
    URL "${_cuobj_url}"
    URL_HASH "SHA256=${_cuobj_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(cuobjclient)

  set(_cuobj_inc
    "${cuobjclient_SOURCE_DIR}/include")
  set(_cuobj_libdir
    "${cuobjclient_SOURCE_DIR}/lib")

  find_library(CUOBJECT_CLIENT_LIBRARY
    NAMES cuobjclient
    PATHS "${_cuobj_libdir}"
    NO_DEFAULT_PATH)

  if(EXISTS "${_cuobj_inc}/cuobjclient.h")
    set(CUOBJECT_CLIENT_INCLUDE_DIR
      "${_cuobj_inc}")
  endif()

  if(CUOBJECT_CLIENT_LIBRARY
      AND CUOBJECT_CLIENT_INCLUDE_DIR)
    set(CUOBJECT_CLIENT_FOUND TRUE)
    add_library(cuobjclient::cuobjclient
      SHARED IMPORTED)
    set_target_properties(
      cuobjclient::cuobjclient PROPERTIES
      IMPORTED_LOCATION
        "${CUOBJECT_CLIENT_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES
        "${CUOBJECT_CLIENT_INCLUDE_DIR}")
    message(STATUS
      "cuObject client: "
      "${CUOBJECT_CLIENT_LIBRARY}")
    message(STATUS
      "cuObject client headers: "
      "${CUOBJECT_CLIENT_INCLUDE_DIR}")
  else()
    set(CUOBJECT_CLIENT_FOUND FALSE)
    message(WARNING
      "cuObject client fetched but library "
      "or headers not found in extracted "
      "archive.")
  endif()

  unset(_cuobj_platform)
  unset(_cuobj_sha256)
  unset(_cuobj_base)
  unset(_cuobj_path)
  unset(_cuobj_archive)
  unset(_cuobj_url)
  unset(_cuobj_inc)
  unset(_cuobj_libdir)
endif()

# ---- cuObject Server (system search) -------

option(HIPOBJ_FIND_CUOBJECT_SERVER
  "Find system-installed cuObject server" OFF)

if(HIPOBJ_FIND_CUOBJECT_SERVER)
  set(_cuobjsrv_search_paths
    ${CUOBJSERVER_ROOT}
    ${CUOBJSERVER_ROOT}/lib
    ${CUOBJSERVER_ROOT}/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/lib/aarch64-linux-gnu
    /usr/local/lib
    /opt/nvidia/cuobjserver/lib)

  set(_cuobjsrv_hdr_paths
    ${CUOBJSERVER_ROOT}/include
    ${CUOBJSERVER_ROOT}/targets
    /usr/include
    /usr/local/include
    /opt/nvidia/cuobjserver/include)

  find_library(CUOBJECT_SERVER_LIBRARY
    NAMES cuobjserver
    PATHS ${_cuobjsrv_search_paths}
    PATH_SUFFIXES lib lib64)

  find_path(CUOBJECT_SERVER_INCLUDE_DIR
    NAMES cuobjserver.h
    PATHS ${_cuobjsrv_hdr_paths}
    PATH_SUFFIXES include)

  if(CUOBJECT_SERVER_LIBRARY
      AND CUOBJECT_SERVER_INCLUDE_DIR)
    set(CUOBJECT_SERVER_FOUND TRUE)
    add_library(cuobjserver::cuobjserver
      SHARED IMPORTED)
    set_target_properties(
      cuobjserver::cuobjserver PROPERTIES
      IMPORTED_LOCATION
        "${CUOBJECT_SERVER_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES
        "${CUOBJECT_SERVER_INCLUDE_DIR}")
    message(STATUS
      "cuObject server: "
      "${CUOBJECT_SERVER_LIBRARY}")
    message(STATUS
      "cuObject server headers: "
      "${CUOBJECT_SERVER_INCLUDE_DIR}")
  else()
    set(CUOBJECT_SERVER_FOUND FALSE)
    message(WARNING
      "cuObject server library not found. "
      "Set CUOBJSERVER_ROOT to its install "
      "prefix, or install the cuobjserver "
      "package from NVIDIA. The server "
      "library is NOT on the public CUDA CDN "
      "and must be obtained separately.")
  endif()

  unset(_cuobjsrv_search_paths)
  unset(_cuobjsrv_hdr_paths)
endif()
