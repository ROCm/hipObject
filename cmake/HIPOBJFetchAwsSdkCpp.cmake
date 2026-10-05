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
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    aws-sdk-cpp
    GIT_REPOSITORY
      https://github.com/aws/aws-sdk-cpp.git
    GIT_TAG 1.11.891
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(aws-sdk-cpp)
endif()
