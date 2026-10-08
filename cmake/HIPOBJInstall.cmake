# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install and package hipObject with rocm-cmake

include_guard(GLOBAL)

# From rocm-cmake (see HIPOBJUseROCmCMake.cmake)
include(ROCMInstallTargets)
include(ROCMCreatePackage)

# Components, which rocm_create_package() turns into packages:
#   runtime  What programs need at run time: the versioned shared
#            library (libhipobj.so.<major> and libhipobj.so.<version>),
#            the license, and the examples. A static build puts the
#            license in devel instead.
#   devel    What's needed to build against hipObject: the
#            libhipobj.so symlink used at link time, the static
#            library, the header, and the CMake package files

# Let the installed library find libamdhip64 and libhsa-runtime64 even
# when they aren't in the loader's search path:
#   - rocm_install() adds $ORIGIN/../lib, which finds them when
#     hipObject is installed into the ROCm prefix
#   - INSTALL_RPATH_USE_LINK_PATH adds the directory they were linked
#     from (the ROCm lib directory)
# The installed examples get their run path from
# hipobj_install_executable().
set_target_properties(hipobj PROPERTIES
  INSTALL_RPATH_USE_LINK_PATH ON)

rocm_install(TARGETS hipobj)

rocm_install(
  FILES ${PROJECT_SOURCE_DIR}/include/hipobj.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hipobj
)

# The static library's target links hip::host,
# hsa-runtime64::hsa-runtime64, and Threads::Threads, so
# hipobj-config.cmake has to find them before it loads the targets file.
# The shared library records these dependencies itself and its target
# doesn't refer to them.
rocm_export_targets(
  TARGETS hipobj::hipobj
  NAMESPACE hipobj::
  STATIC_DEPENDS
    PACKAGE hip CONFIG
    PACKAGE hsa-runtime64 CONFIG
    PACKAGE Threads
)

# Package dependencies
#
# The packages depend on both the ROCm runtime and development
# packages until there are separate release and development builds.
#
# libibverbs isn't a dependency, since hipObject loads it with dlopen().
if(ROCM_VERSION VERSION_GREATER_EQUAL 7.11)
  # rocm-core is a meta-package all legacy ROCm packages depend on. It
  # doesn't exist in the current ROCm package repository, so suppress
  # the dependency on it that rocm-cmake adds.
  set(ROCM_DEP_ROCMCORE OFF CACHE BOOL "" FORCE)
  set(HIPOBJ_ROCM_MM
    "${HIPOBJ_ROCM_VERSION_MAJOR}.${HIPOBJ_ROCM_VERSION_MINOR}")
  rocm_package_add_deb_dependencies(
    DEPENDS "amdrocm-runtime-dev${HIPOBJ_ROCM_MM}")
  rocm_package_add_rpm_dependencies(
    DEPENDS "amdrocm-runtime-devel${HIPOBJ_ROCM_MM}")
else()
  rocm_package_add_dependencies(DEPENDS hip-runtime-amd)
  rocm_package_add_deb_dependencies(DEPENDS hip-dev)
  rocm_package_add_rpm_dependencies(DEPENDS hip-devel)
endif()

# The system libraries the installed programs link (see
# hipobj_add_runtime_library_dependency()). These go in the package-wide
# dependencies, which only the runtime package uses: the devel package
# has its own. A static build doesn't package the programs, and its one
# package uses the package-wide dependencies, so leave them out.
if(BUILD_SHARED_LIBS)
  get_property(HIPOBJ_RUNTIME_DEB_DEPENDS GLOBAL
    PROPERTY HIPOBJ_RUNTIME_DEB_DEPENDS)
  get_property(HIPOBJ_RUNTIME_RPM_DEPENDS GLOBAL
    PROPERTY HIPOBJ_RUNTIME_RPM_DEPENDS)
  if(HIPOBJ_RUNTIME_DEB_DEPENDS)
    list(REMOVE_DUPLICATES HIPOBJ_RUNTIME_DEB_DEPENDS)
    rocm_package_add_deb_dependencies(DEPENDS ${HIPOBJ_RUNTIME_DEB_DEPENDS})
  endif()
  if(HIPOBJ_RUNTIME_RPM_DEPENDS)
    list(REMOVE_DUPLICATES HIPOBJ_RUNTIME_RPM_DEPENDS)
    rocm_package_add_rpm_dependencies(DEPENDS ${HIPOBJ_RUNTIME_RPM_DEPENDS})
  endif()
endif()

# CPack license setup
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.md")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")

# rocm-cmake sets CPACK_SET_DESTDIR on Linux, which conflicts with
# asking for relocatable packages
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
set(CPACK_DEB_PACKAGE_RELOCATABLE OFF)

# Don't put build ID links in the RPM packages
#
# When a binary has a build ID (GCC's linker adds one by default),
# rpmbuild adds a /usr/lib/.build-id/<xx>/<id> link to it in the
# package. Those are outside the install prefix, so installing another
# package with the same build ID, such as a different hipObject build
# for another ROCm version, is a file conflict. These links are only
# used to find debug info, and there's no debuginfo package.
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none")

# Let RPM generate the package's Provides from the libraries it
# contains, so packages that depend on libhipobj.so.<major> instead of
# a package name are satisfied. rocm_create_package() turns off
# AUTOREQPROV unless it's already defined, and AUTOREQPROV overrides
# AUTOREQ and AUTOPROV, so define it as empty. The requirements are
# still listed explicitly, as rocm-cmake intends, instead of generated.
# Ref: https://ftp.rpm.org/max-rpm/s1-rpm-specref-preamble.html#S3-RPM-SPECREF-AUTOREQPROV
set(CPACK_RPM_PACKAGE_AUTOREQPROV "")
set(CPACK_RPM_PACKAGE_AUTOPROV ON)
set(CPACK_RPM_PACKAGE_AUTOREQ OFF)

# Don't claim ownership of the directories ROCm provides
#
# CPack adds a %dir entry for every parent directory of an installed
# file, so installing into ROCm's prefix makes the package a co-owner
# of ROCm's directories. RPM only allows that when the mode, owner, and
# group match exactly, and declares a file conflict otherwise. ROCm
# 7.14+ sets the setgid bit on its directories, which CPack can't do,
# so leave these directories to ROCm (a dependency, so they exist).
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  "/opt"
  "/opt/rocm"
  "${CMAKE_INSTALL_PREFIX}"
  "${CMAKE_INSTALL_PREFIX}/bin"
  "${CMAKE_INSTALL_PREFIX}/include"
  "${CMAKE_INSTALL_PREFIX}/lib"
  "${CMAKE_INSTALL_PREFIX}/lib/cmake"
  "${CMAKE_INSTALL_PREFIX}/share"
  "${CMAKE_INSTALL_PREFIX}/share/doc"
)

# Package release information
#
# rocm_create_package() takes the release from the
# CPACK_DEBIAN_PACKAGE_RELEASE and CPACK_RPM_PACKAGE_RELEASE environment
# variables, which ROCm's release builds set, falling back to
# PROJECT_VERSION_TWEAK. It appends the ROCM_LIBPATCH_VERSION
# environment variable (ROCM_VERSION with each '.' replaced by '0') to
# the package version.
# See https://github.com/ROCm/ROCm/blob/8aa43d132f0d541eb5303dc532f5931cb80ad87a/tools/rocm-build/envsetup.sh#L77-L94
if(NOT DEFINED ENV{CPACK_DEBIAN_PACKAGE_RELEASE}
    AND NOT DEFINED ENV{CPACK_RPM_PACKAGE_RELEASE})
  set(PROJECT_VERSION_TWEAK "local")
endif()

# Include the ROCm version in the package version, even outside of a
# release build
if(NOT DEFINED ENV{ROCM_LIBPATCH_VERSION})
  string(REPLACE "." "0" HIPOBJ_ROCM_LIBPATCH_VERSION "${ROCM_VERSION}")
  set(ENV{ROCM_LIBPATCH_VERSION} "${HIPOBJ_ROCM_LIBPATCH_VERSION}")
endif()

# Create the packages. A static build only has a devel package, since
# there's no shared library for the runtime package.
rocm_create_package(
  NAME "hipObject"
  DESCRIPTION "The hipObject library"
  MAINTAINER "hipobject-maintainer@amd.com"
)
