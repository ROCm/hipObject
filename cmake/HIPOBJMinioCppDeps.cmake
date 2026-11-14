# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Fetch and expose minio-cpp dependencies using vcpkg-style
# imported target names (unofficial::curlpp::curlpp, etc.)
#
# Dependencies that FetchContent_MakeAvailable() adds to the build are
# declared SYSTEM, and the ones built here have SYSTEM include
# directories, so warnings in their headers don't fire in hipObject
# targets that use them.

include(FetchContent)

# The packages' dependencies name OpenSSL 3's libraries
find_package(OpenSSL 3 REQUIRED)
if(NOT OpenSSL_FOUND)
  message(FATAL_ERROR
    "HIPOBJ_MINIO_CLIENT requires OpenSSL development headers. "
    "On Ubuntu: sudo apt install libssl-dev zlib1g-dev libcurl4-openssl-dev")
endif()
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

# FetchContent_MakeAvailable() uses an installed nlohmann_json or
# pugixml if find_package() finds one (FIND_PACKAGE_ARGS), and downloads
# it otherwise. When it downloads one, FIND_PACKAGE_ARGS also makes it
# write package files to CMAKE_FIND_PACKAGE_REDIRECTS_DIR, so that
# minio-cpp's own find_package() calls find the downloaded copy.
# FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER would drop FIND_PACKAGE_ARGS,
# and with it those package files, so minio-cpp would find an installed
# copy, or none.
if(FETCHCONTENT_TRY_FIND_PACKAGE_MODE STREQUAL "NEVER")
  message(FATAL_ERROR
    "HIPOBJ_MINIO_CLIENT doesn't build with "
    "FETCHCONTENT_TRY_FIND_PACKAGE_MODE set to NEVER, since minio-cpp "
    "wouldn't find the nlohmann_json and pugixml that the build downloads")
endif()
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  SYSTEM
  FIND_PACKAGE_ARGS CONFIG QUIET
)
FetchContent_Declare(
  pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG v1.14
  SYSTEM
  FIND_PACKAGE_ARGS CONFIG QUIET
)
FetchContent_MakeAvailable(nlohmann_json)

# A downloaded pugixml is only a build dependency of the bridge, like
# minio-cpp (see integrations/minio-cpp/CMakeLists.txt), but its own
# build files install it, and v1.14 has no option to stop that. So it's
# added with CMAKE_SKIP_INSTALL_RULES on, which keeps its directory from
# getting an install script, and EXCLUDE_FROM_ALL, which keeps ours from
# including the script it doesn't have. A pugixml that something else
# found or added first, such as a superproject, is left alone.
# (nlohmann_json only installs itself when it's the top-level project.)
FetchContent_GetProperties(pugixml)
set(_hipobj_pugixml_was_populated "${pugixml_POPULATED}")
set(_hipobj_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(pugixml)
set(CMAKE_SKIP_INSTALL_RULES "${_hipobj_skip_install_rules}")
if(NOT _hipobj_pugixml_was_populated AND pugixml_SOURCE_DIR)
  set_property(DIRECTORY ${pugixml_SOURCE_DIR}
    PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
if(NOT TARGET pugixml::pugixml AND TARGET pugixml)
  add_library(pugixml::pugixml ALIAS pugixml)
endif()

# inih and curlpp are built here, not with their own build files, so
# SOURCE_SUBDIR names a directory that doesn't exist, which keeps
# FetchContent_MakeAvailable() from adding them to the build. It only
# downloads them.
if(NOT TARGET unofficial::inih::inireader)
  FetchContent_Declare(
    inih
    GIT_REPOSITORY https://github.com/benhoyt/inih.git
    GIT_TAG r57
    SOURCE_SUBDIR "DISABLE ADDING TO BUILD"
  )
  FetchContent_MakeAvailable(inih)
  add_library(inih_c STATIC ${inih_SOURCE_DIR}/ini.c)
  target_include_directories(inih_c SYSTEM PUBLIC ${inih_SOURCE_DIR})
  add_library(inih_cpp STATIC ${inih_SOURCE_DIR}/cpp/INIReader.cpp)
  target_include_directories(inih_cpp SYSTEM PUBLIC
    ${inih_SOURCE_DIR}
    ${inih_SOURCE_DIR}/cpp)
  target_link_libraries(inih_cpp PUBLIC inih_c)
  add_library(unofficial::inih::inireader ALIAS inih_cpp)
endif()

if(NOT TARGET unofficial::curlpp::curlpp)
  find_path(CURLPP_INCLUDE_DIR curlpp/Easy.hpp
    PATHS /usr/include /usr/local/include)
  find_library(CURLPP_LIBRARY curlpp)
  if(CURLPP_INCLUDE_DIR AND CURLPP_LIBRARY)
    # find_library() prefers the shared library, but takes the static
    # one if that's all there is
    add_library(unofficial-curlpp UNKNOWN IMPORTED GLOBAL)
    set_target_properties(unofficial-curlpp PROPERTIES
      IMPORTED_LOCATION "${CURLPP_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CURLPP_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "CURL::libcurl")
    add_library(unofficial::curlpp::curlpp ALIAS unofficial-curlpp)
  else()
    # v0.8.1, the latest release, doesn't compile against libcurl 8.10
    # or later, which defines CURLOPT_CLOSEPOLICY as a plain int rather
    # than a CURLoption. Upstream removed that option after the release,
    # so pin a later commit on master.
    FetchContent_Declare(
      curlpp
      GIT_REPOSITORY https://github.com/jpbarrette/curlpp.git
      GIT_TAG ec1b66e699557cd9d608d322c013a1ebda16bd08
      SOURCE_SUBDIR "DISABLE ADDING TO BUILD"
    )
    FetchContent_MakeAvailable(curlpp)
    file(GLOB_RECURSE CURLPP_SOURCES ${curlpp_SOURCE_DIR}/src/*.cpp)
    add_library(curlpp_built STATIC ${CURLPP_SOURCES})
    target_include_directories(curlpp_built SYSTEM PUBLIC
      ${curlpp_SOURCE_DIR}/include)
    target_compile_options(curlpp_built PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(curlpp_built PUBLIC CURL::libcurl)
    add_library(unofficial::curlpp::curlpp ALIAS curlpp_built)
  endif()
endif()

if(TARGET pugixml::pugixml)
  set(HIPOBJ_MINIO_PUGIXML_TARGET pugixml::pugixml)
elseif(TARGET pugixml)
  set(HIPOBJ_MINIO_PUGIXML_TARGET pugixml)
else()
  message(FATAL_ERROR "pugixml target not found")
endif()
