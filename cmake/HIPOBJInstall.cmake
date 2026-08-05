# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install rules for hipObject

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS hipobj
  EXPORT hipobj-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  COMPONENT hipobj
)

install(FILES
  ${CMAKE_SOURCE_DIR}/include/hipobj.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hipobj
  COMPONENT hipobj
)

set(HIPOBJ_CONFIG_INSTALL_DIR
  ${CMAKE_INSTALL_LIBDIR}/cmake/hipobj)

install(EXPORT hipobj-targets
  FILE hipobj-targets.cmake
  NAMESPACE hipobj::
  DESTINATION ${HIPOBJ_CONFIG_INSTALL_DIR}
  COMPONENT hipobj
)

configure_package_config_file(
  ${CMAKE_SOURCE_DIR}/cmake/hipobj-config.cmake.in
  ${CMAKE_BINARY_DIR}/hipobj-config.cmake
  INSTALL_DESTINATION ${HIPOBJ_CONFIG_INSTALL_DIR}
  PATH_VARS
    CMAKE_INSTALL_INCLUDEDIR
    CMAKE_INSTALL_LIBDIR
)

write_basic_package_version_file(
  ${CMAKE_BINARY_DIR}/hipobj-config-version.cmake
  VERSION ${HIPOBJ_LIBRARY_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_BINARY_DIR}/hipobj-config.cmake
  ${CMAKE_BINARY_DIR}/hipobj-config-version.cmake
  DESTINATION ${HIPOBJ_CONFIG_INSTALL_DIR}
  COMPONENT hipobj
)
