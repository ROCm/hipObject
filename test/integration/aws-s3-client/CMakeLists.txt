# SPDX-License-Identifier: MIT

include(HIPOBJFetchAwsSdkCpp)

# hipObject's helpers set the C++ standard, the warning, -Werror and
# hardening flags, and the sanitizer, coverage, clang-tidy and IWYU hooks
hipobj_add_library(
  NAME hipobj-aws-s3-client
  TYPE STATIC
  SRCS hipobj_aws_s3_client.cpp)

target_include_directories(hipobj-aws-s3-client PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
# log.h prints with shared/hipobj-print.h
target_include_directories(hipobj-aws-s3-client PRIVATE
  ${PROJECT_SOURCE_DIR}/shared)

target_link_libraries(hipobj-aws-s3-client PUBLIC
  hipobj
  aws-cpp-sdk-s3)

# aws-sdk-cpp's own CMake uses directory-scoped include_directories()
# rather than target_include_directories(), so it never propagates to
# targets outside its own subdirectory tree via target_link_libraries().
# When fetched via FetchContent (aws-sdk-cpp_SOURCE_DIR set by
# HIPOBJFetchAwsSdkCpp.cmake), add its headers explicitly; a
# system-installed AWSSDK::aws-cpp-sdk-s3 already exports its own.
if(DEFINED aws-sdk-cpp_SOURCE_DIR)
  # aws-sdk-cpp's own aws-cpp-sdk-core/aws-cpp-sdk-s3 targets rely on
  # directory-scoped include_directories() (set by the umbrella build)
  # rather than target_include_directories(), and their link to
  # aws-crt-cpp does not re-export it as PUBLIC either, so none of this
  # reaches a target outside the SDK's own subdirectory tree via
  # target_link_libraries(). Add every header root by hand instead, as
  # SYSTEM, so the warning flags don't apply to the SDK's headers.
  target_include_directories(hipobj-aws-s3-client SYSTEM PUBLIC
    ${aws-sdk-cpp_SOURCE_DIR}/src/aws-cpp-sdk-core/include
    ${aws-sdk-cpp_SOURCE_DIR}/generated/src/aws-cpp-sdk-s3/include
    ${aws-sdk-cpp_SOURCE_DIR}/crt/aws-crt-cpp/include)

  # aws-crt-cpp itself sits on the C aws-c-*/aws-checksums/s2n libraries;
  # those ARE proper modern CMake targets with real
  # INTERFACE_INCLUDE_DIRECTORIES, so pull those in directly rather than
  # continuing to hand-derive paths one compile error at a time.
  foreach(_hipobj_awssdk_crt_target IN ITEMS
      aws-c-common aws-c-cal aws-c-io aws-c-compression aws-c-http
      aws-c-mqtt aws-c-auth aws-c-s3 aws-c-event-stream aws-c-sdkutils
      aws-checksums s2n)
    if(TARGET ${_hipobj_awssdk_crt_target})
      get_target_property(_hipobj_awssdk_crt_includes
        ${_hipobj_awssdk_crt_target} INTERFACE_INCLUDE_DIRECTORIES)
      if(_hipobj_awssdk_crt_includes)
        target_include_directories(hipobj-aws-s3-client SYSTEM PUBLIC
          ${_hipobj_awssdk_crt_includes})
      endif()
    endif()
  endforeach()
  unset(_hipobj_awssdk_crt_target)
  unset(_hipobj_awssdk_crt_includes)
endif()

hipobj_add_executable(
  NAME hipobj-aws-s3-demo
  SRCS aws_s3_demo.cpp)
if(HIPOBJ_CPU_RDMA)
  target_compile_definitions(hipobj-aws-s3-demo PRIVATE HIPOBJ_CPU_RDMA)
endif()
target_link_libraries(hipobj-aws-s3-demo PRIVATE
  hipobj-aws-s3-client
  aws-cpp-sdk-core
  Threads::Threads)
if(NOT HIPOBJ_CPU_RDMA)
  target_link_libraries(hipobj-aws-s3-demo PRIVATE hip::host)
endif()

# The static system libcrypto.a pulls in zstd and jitterentropy symbols
# that its own dependency chain does not otherwise link; make the link
# explicit (requires libzstd + libjitterentropy3-dev to be installed).
find_library(HIPOBJ_ZSTD_LIB zstd)
if(HIPOBJ_ZSTD_LIB)
  target_link_libraries(hipobj-aws-s3-demo PRIVATE ${HIPOBJ_ZSTD_LIB})
endif()
find_library(HIPOBJ_JITTERENTROPY_LIB jitterentropy)
if(HIPOBJ_JITTERENTROPY_LIB)
  target_link_libraries(hipobj-aws-s3-demo PRIVATE ${HIPOBJ_JITTERENTROPY_LIB})
endif()
