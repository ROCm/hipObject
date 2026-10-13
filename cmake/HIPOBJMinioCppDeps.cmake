# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Fetch and expose minio-cpp dependencies using vcpkg-style
# imported target names (unofficial::curlpp::curlpp, etc.)
#
# Dependencies added with FetchContent_MakeAvailable() are declared
# SYSTEM, so warnings in their headers don't fire in hipObject targets
# that use them.

include(FetchContent)

find_package(OpenSSL REQUIRED)
if(NOT OpenSSL_FOUND)
  message(FATAL_ERROR
    "HIPOBJ_MINIO_CLIENT requires OpenSSL development headers. "
    "On Ubuntu: sudo apt install libssl-dev zlib1g-dev libcurl4-openssl-dev")
endif()
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    SYSTEM
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(pugixml CONFIG QUIET)
if(NOT pugixml_FOUND)
  FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.14
    SYSTEM
  )
  set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(pugixml)
  if(NOT TARGET pugixml::pugixml AND TARGET pugixml)
    add_library(pugixml::pugixml ALIAS pugixml)
  endif()
endif()

if(NOT TARGET unofficial::inih::inireader)
  FetchContent_Declare(
    inih
    GIT_REPOSITORY https://github.com/benhoyt/inih.git
    GIT_TAG r57
  )
  FetchContent_GetProperties(inih)
  if(NOT inih_POPULATED)
    FetchContent_Populate(inih)
    add_library(inih_c STATIC ${inih_SOURCE_DIR}/ini.c)
    target_include_directories(inih_c SYSTEM PUBLIC ${inih_SOURCE_DIR})
    add_library(inih_cpp STATIC ${inih_SOURCE_DIR}/cpp/INIReader.cpp)
    target_include_directories(inih_cpp SYSTEM PUBLIC
      ${inih_SOURCE_DIR}
      ${inih_SOURCE_DIR}/cpp)
    target_link_libraries(inih_cpp PUBLIC inih_c)
    add_library(unofficial::inih::inireader ALIAS inih_cpp)
  endif()
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
    )
    FetchContent_GetProperties(curlpp)
    if(NOT curlpp_POPULATED)
      FetchContent_Populate(curlpp)
      file(GLOB_RECURSE CURLPP_SOURCES ${curlpp_SOURCE_DIR}/src/*.cpp)
      add_library(curlpp_built STATIC ${CURLPP_SOURCES})
      target_include_directories(curlpp_built SYSTEM PUBLIC
        ${curlpp_SOURCE_DIR}/include)
      target_compile_options(curlpp_built PRIVATE -Wno-deprecated-declarations)
      target_link_libraries(curlpp_built PUBLIC CURL::libcurl)
      add_library(unofficial::curlpp::curlpp ALIAS curlpp_built)
    endif()
  endif()
endif()

if(TARGET pugixml::pugixml)
  set(HIPOBJ_MINIO_PUGIXML_TARGET pugixml::pugixml)
elseif(TARGET pugixml)
  set(HIPOBJ_MINIO_PUGIXML_TARGET pugixml)
else()
  message(FATAL_ERROR "pugixml target not found")
endif()
