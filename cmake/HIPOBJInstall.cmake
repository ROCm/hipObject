# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install rules for hipObject

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Let the installed library find libamdhip64 and libhsa-runtime64 even
# when they aren't in the loader's search path:
#   - $ORIGIN/../lib finds them when hipObject is installed into the
#     ROCm prefix
#   - INSTALL_RPATH_USE_LINK_PATH adds the directory they were linked
#     from (the ROCm lib directory)
# The installed examples get their run path from
# hipobj_install_executable().
set_target_properties(hipobj PROPERTIES
  INSTALL_RPATH "\$ORIGIN/../lib"
  INSTALL_RPATH_USE_LINK_PATH ON)

# Components:
#   hipobj      What programs need at run time: the versioned shared
#               library (libhipobj.so.<major> and libhipobj.so.<version>)
#   hipobj-dev  What's needed to build against hipObject: the
#               libhipobj.so symlink used at link time, the static
#               library, the header, and the CMake package files
install(TARGETS hipobj
  EXPORT hipobj-targets
  ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT hipobj-dev
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT hipobj
    NAMELINK_COMPONENT hipobj-dev
)

install(FILES
  ${CMAKE_SOURCE_DIR}/include/hipobj.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hipobj
  COMPONENT hipobj-dev
)

set(HIPOBJ_CONFIG_INSTALL_DIR
  ${CMAKE_INSTALL_LIBDIR}/cmake/hipobj)

install(EXPORT hipobj-targets
  FILE hipobj-targets.cmake
  NAMESPACE hipobj::
  DESTINATION ${HIPOBJ_CONFIG_INSTALL_DIR}
  COMPONENT hipobj-dev
)

# hipobj-config.cmake only finds hipobj's dependencies for the static
# library
get_target_property(HIPOBJ_LIBRARY_TYPE hipobj TYPE)

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
  COMPONENT hipobj-dev
)
