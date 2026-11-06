# SPDX-License-Identifier: MIT

# Find or fetch the AWS SDK for C++ (S3 component only), used by
# test/integration/aws-s3-client to build a hipObject-aware S3 client.

include(FetchContent)

find_package(AWSSDK QUIET COMPONENTS s3)
if(NOT AWSSDK_FOUND)
  message(STATUS
    "AWS SDK for C++ not found, fetching from GitHub")

  set(BUILD_ONLY "s3" CACHE STRING "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(AUTORUN_UNIT_TESTS OFF CACHE BOOL "" FORCE)

  # The SDK is built static. Its CMake sets CMP0077 to OLD, so its
  # option(BUILD_SHARED_LIBS) only sees a cache entry, not a normal
  # variable: set the cache entry for the SDK's configure, and put back
  # what was there afterwards, so the rest of the project, and a project
  # above it, build as they were configured.
  if(DEFINED CACHE{BUILD_SHARED_LIBS})
    set(_hipobj_aws_shared_libs "$CACHE{BUILD_SHARED_LIBS}")
    get_property(_hipobj_aws_shared_libs_doc
      CACHE BUILD_SHARED_LIBS PROPERTY HELPSTRING)
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    aws-sdk-cpp
    GIT_REPOSITORY
      https://github.com/aws/aws-sdk-cpp.git
    GIT_TAG 1.11.891
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(aws-sdk-cpp)

  if(DEFINED _hipobj_aws_shared_libs)
    set(BUILD_SHARED_LIBS "${_hipobj_aws_shared_libs}"
      CACHE BOOL "${_hipobj_aws_shared_libs_doc}" FORCE)
  else()
    unset(BUILD_SHARED_LIBS CACHE)
  endif()
  unset(_hipobj_aws_shared_libs)
  unset(_hipobj_aws_shared_libs_doc)
endif()
