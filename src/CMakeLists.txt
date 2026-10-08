# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# hipObject library

set(HIPOBJ_SOURCES
  hipobj.cpp
  state.cpp
  buffer.cpp
  common/hip-seam.cpp
  common/ibv-wrapper.cpp
  common/rdma-topology.cpp
  common/rdma-numa-wrapper.cpp
  rdma/transport.cpp
  rdma/token.cpp
  s3/control.cpp)

if(HIPOBJECT_V2_API)
  list(APPEND HIPOBJ_SOURCES
    rdma/v2-wire.cpp
    rdma/v2-state.cpp
    rdma/v2-clock.cpp
    rdma/v2-random.cpp
    rdma/v2-registry.cpp
    rdma/v2-transport.cpp)
endif()

if(HIPOBJ_BNXT)
  list(APPEND HIPOBJ_SOURCES
    rdma/bnxt/bnxt-backend.cpp)
endif()
if(HIPOBJ_IONIC)
  list(APPEND HIPOBJ_SOURCES
    rdma/ionic/ionic-backend.cpp)
endif()

hipobj_add_library(
  NAME hipobj
  SRCS ${HIPOBJ_SOURCES})

# Only the functions marked HIPOBJ_API in include/hipobj.h are exported
# from the shared library. VISIBILITY_INLINES_HIDDEN also hides inline
# member functions. Neither hides C++ standard library template
# instantiations, since libstdc++ declares them with default
# visibility, so the version script makes everything that isn't
# hipObj* local. Visibility has no effect on the static library, and
# static libraries aren't linked. test/exports checks the result.
set_target_properties(hipobj PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

get_target_property(HIPOBJ_LIBRARY_TYPE hipobj TYPE)
if(HIPOBJ_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
  target_link_options(hipobj PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/hipobj.map")
  # Relink when the version script changes
  set_property(TARGET hipobj APPEND PROPERTY
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hipobj.map)

  # Fail the link if the library uses a symbol that none of its
  # dependencies define, instead of leaving it for the program that
  # loads it. Clang doesn't link the sanitizer runtimes into shared
  # libraries (the executable provides them), so sanitizer builds with
  # Clang always have undefined symbols. GCC links them as shared
  # libraries, so its sanitizer builds are fine.
  if(NOT (HIPOBJ_USE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    target_link_options(hipobj PRIVATE "LINKER:--no-undefined")
  endif()
endif()

target_include_directories(hipobj
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/hipobj>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/shared
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/rdma
    ${CMAKE_CURRENT_SOURCE_DIR}/s3)

# hipobj.h includes no HIP or HSA headers, so consumers don't need their
# include directories or compile definitions. For the static library,
# CMake still passes PRIVATE dependencies on to the final link.
target_link_libraries(hipobj
  PRIVATE
    hip::host
    hsa-runtime64::hsa-runtime64
    dl
    Threads::Threads)

# A shared library records its own dependency on the C++ runtime. The
# static library doesn't, so a consumer linking with the C compiler
# would otherwise miss it.
if(HIPOBJ_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
  target_link_libraries(hipobj INTERFACE stdc++)
endif()

if(HIPOBJ_BNXT)
  target_compile_definitions(hipobj
    PRIVATE HIPOBJ_BNXT)
endif()
if(HIPOBJ_IONIC)
  target_compile_definitions(hipobj
    PRIVATE HIPOBJ_IONIC)
endif()
if(HIPOBJECT_V2_API)
  # PUBLIC: include/hipobj.h only declares the V2 API surface when this
  # macro is defined, so consumers linking hipobj::hipobj (in-tree or via
  # the exported CMake package) need it on their own translation units too.
  target_compile_definitions(hipobj
    PUBLIC HIPOBJECT_V2_API)
endif()

# The unit tests compile these same sources into a separate object
# library (see test/unit/CMakeLists.txt), so export the list to the
# parent scope as absolute paths that are valid from any directory.
list(TRANSFORM HIPOBJ_SOURCES
  PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(HIPOBJ_SOURCES ${HIPOBJ_SOURCES} PARENT_SCOPE)
