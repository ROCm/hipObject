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
#            the license, the tools (HIPOBJ_INSTALL_TOOLS), and with
#            HIPOBJ_MINIO_CLIENT, the MinIO example program. A static
#            build puts the license in devel instead, and doesn't
#            package the programs.
#   devel    What's needed to build against hipObject: the
#            libhipobj.so symlink used at link time, the static
#            library, the header, the CMake package files, and the
#            examples' sources

# Let the installed library find libamdhip64 and libhsa-runtime64 even
# when they aren't in the loader's search path:
#   - rocm_install() adds $ORIGIN/../lib, which finds them when
#     hipObject is installed into the ROCm prefix
#   - INSTALL_RPATH_USE_LINK_PATH adds the directory they were linked
#     from (the ROCm lib directory)
# The installed programs get their run path from
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
# rocm-core is a meta-package all legacy ROCm packages depend on. It
# doesn't exist in the current ROCm package repository, so suppress the
# dependency on it that rocm-cmake adds.
set(ROCM_DEP_ROCMCORE OFF CACHE BOOL "" FORCE)
set(HIPOBJ_ROCM_MM
  "${HIPOBJ_ROCM_VERSION_MAJOR}.${HIPOBJ_ROCM_VERSION_MINOR}")
rocm_package_add_deb_dependencies(
  DEPENDS "amdrocm-runtime-dev${HIPOBJ_ROCM_MM}")
rocm_package_add_rpm_dependencies(
  DEPENDS "amdrocm-runtime-devel${HIPOBJ_ROCM_MM}")

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

# libibverbs is a weak dependency (Recommends), of the package that
# holds the library: the runtime package, or a static build's one
# package. hipObject loads it with dlopen(), by its soname
# (libibverbs.so.1), so it needs the runtime package (libibverbs1 or
# libibverbs) and not the development package. hipObjInit() fails
# without it, but hipObject can be installed and built against without
# it, such as on a build machine with no RDMA NIC. Debian's libibverbs1
# recommends ibverbs-providers, which has the NIC drivers. The RPM
# package names the soname, like its other dependencies.
#
# CPack only writes an RPM Recommends from CMake 4.1, so an RPM package
# built with an earlier CMake doesn't recommend libibverbs. (Before 4.0,
# CPack doesn't write any weak dependency, including the Suggests that
# rocm_create_package() adds, since its check for rpmbuild's support of
# them runs an rpm command it never finds.)
if(BUILD_SHARED_LIBS)
  set(HIPOBJ_LIBRARY_COMPONENT RUNTIME)
else()
  set(HIPOBJ_LIBRARY_COMPONENT DEVEL)
endif()
set(HIPOBJ_LIBIBVERBS_DEB libibverbs1)
if(CMAKE_VERSION VERSION_LESS 4.1)
  set(HIPOBJ_LIBIBVERBS_RPM)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(HIPOBJ_LIBIBVERBS_RPM "libibverbs.so.1()(64bit)")
else()
  set(HIPOBJ_LIBIBVERBS_RPM "libibverbs.so.1")
endif()
# rocm_create_package() appends its own recommendations to the DEB one
set(CPACK_DEBIAN_${HIPOBJ_LIBRARY_COMPONENT}_PACKAGE_RECOMMENDS
  "${HIPOBJ_LIBIBVERBS_DEB}")
if(HIPOBJ_LIBIBVERBS_RPM)
  set(CPACK_RPM_${HIPOBJ_LIBRARY_COMPONENT}_PACKAGE_RECOMMENDS
    "${HIPOBJ_LIBIBVERBS_RPM}")
endif()
# For the package tests (see test/install/CMakeLists.txt)
set_property(GLOBAL PROPERTY
  HIPOBJ_LIBRARY_DEB_RECOMMENDS "${HIPOBJ_LIBIBVERBS_DEB}")
set_property(GLOBAL PROPERTY
  HIPOBJ_LIBRARY_RPM_RECOMMENDS "${HIPOBJ_LIBIBVERBS_RPM}")

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

# Package version and release
#
# rocm_create_package() takes the release from the
# CPACK_DEBIAN_PACKAGE_RELEASE and CPACK_RPM_PACKAGE_RELEASE environment
# variables, which ROCm's release builds set, and uses it as it is. Set
# whichever isn't set to "local", so the DEB release is always known here
# (see the devel package's dependency below). It appends the
# ROCM_LIBPATCH_VERSION environment variable (ROCM_VERSION with each '.'
# replaced by '0') to the package version, so set that too, which
# includes the ROCm version in the package version even outside of a
# release build.
# See https://github.com/ROCm/ROCm/blob/8aa43d132f0d541eb5303dc532f5931cb80ad87a/tools/rocm-build/envsetup.sh#L77-L94
foreach(format IN ITEMS DEBIAN RPM)
  if("$ENV{CPACK_${format}_PACKAGE_RELEASE}" STREQUAL "")
    set(ENV{CPACK_${format}_PACKAGE_RELEASE} "local")
  endif()
endforeach()
set(HIPOBJ_DEB_RELEASE "$ENV{CPACK_DEBIAN_PACKAGE_RELEASE}")
if(NOT DEFINED ENV{ROCM_LIBPATCH_VERSION})
  string(REPLACE "." "0" HIPOBJ_ROCM_LIBPATCH_VERSION "${ROCM_VERSION}")
  set(ENV{ROCM_LIBPATCH_VERSION} "${HIPOBJ_ROCM_LIBPATCH_VERSION}")
endif()
# The package version, as rocm_create_package() makes it
set(HIPOBJ_PACKAGE_VERSION
  "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
set(HIPOBJ_LIBPATCH_VERSION "$ENV{ROCM_LIBPATCH_VERSION}")
if(HIPOBJ_LIBPATCH_VERSION)
  string(APPEND HIPOBJ_PACKAGE_VERSION ".${HIPOBJ_LIBPATCH_VERSION}")
endif()

# Package names
#
# A build is for one ROCm version, so each package's name ends with the
# ROCm major and minor version, as ROCm's own packages do
# (amdrocm-hipfile10.1, amdrocm-hipfile-dev10.1):
#   hipobject<mm>                        The runtime package
#   hipobject-dev<mm> (DEB),             The devel package
#   hipobject-devel<mm> (RPM)
#   hipobject-static-dev<mm> (DEB),      A static build's one package
#   hipobject-static-devel<mm> (RPM)
# Builds for different ROCm versions are then different packages, so a
# package manager never compares their versions, whose ROCm part would
# sort 10.1.0 (100100) before 7.14.1 (701401). Builds that install into
# their ROCm version's own prefix (/opt/rocm/core-<major>.<minor>), as the
# nightly builds do, install side by side.
#
# rocm_create_package() names the runtime package after its NAME, and
# makes the devel package depend on the package with that name, so NAME
# ends with the ROCm version. It keeps package names that are already
# set, so set the devel package's, which would otherwise be
# hipobject<mm>-dev.
set(HIPOBJ_PACKAGE_NAME "hipobject${HIPOBJ_ROCM_MM}")
if(BUILD_SHARED_LIBS)
  set(HIPOBJ_DEVEL_PACKAGE_BASE "hipobject")
else()
  set(HIPOBJ_DEVEL_PACKAGE_BASE "hipobject-static")
endif()
set(CPACK_DEBIAN_DEVEL_PACKAGE_NAME
  "${HIPOBJ_DEVEL_PACKAGE_BASE}-dev${HIPOBJ_ROCM_MM}")
set(CPACK_RPM_DEVEL_PACKAGE_NAME
  "${HIPOBJ_DEVEL_PACKAGE_BASE}-devel${HIPOBJ_ROCM_MM}")

# The devel package depends on the runtime package from the same build.
# rocm_create_package() adds a dependency on any runtime package at
# least as new, without the release, so a nightly's devel package would
# accept any other nightly's runtime package, whose library might not
# match its header. Add a dependency on this exact version and release,
# which it adds its own to.
#
# The RPM one names them with the spec file's %{version} and %{release},
# which rpmbuild expands to the devel package's own, the same as the
# runtime package's. rocm_create_package() appends %{?dist} to the RPM
# release when the build machine's rpm defines a distribution tag (such
# as .el9), so the release isn't known until rpmbuild expands it.
if(BUILD_SHARED_LIBS)
  set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS
    "${HIPOBJ_PACKAGE_NAME} (= ${HIPOBJ_PACKAGE_VERSION}-${HIPOBJ_DEB_RELEASE})")
  set(CPACK_RPM_DEVEL_PACKAGE_REQUIRES
    "${HIPOBJ_PACKAGE_NAME} = %{version}-%{release}")
endif()

# The examples' sources, for users to build against the installed
# hipObject, in the directory that rocm_create_package() installs the
# license in, share/doc/<runtime package name>. CMakeLists.install.cmake
# is their CMakeLists.txt; the one in the source tree uses hipObject's
# build settings. They go in the devel package, which has what they
# build against, and which a static build also has.
if(HIPOBJ_INSTALL_EXAMPLES)
  install(
    FILES
      ${PROJECT_SOURCE_DIR}/examples/README.md
      ${PROJECT_SOURCE_DIR}/examples/get-version.cpp
      ${PROJECT_SOURCE_DIR}/examples/get-object.cpp
      ${PROJECT_SOURCE_DIR}/examples/put-object.cpp
      ${PROJECT_SOURCE_DIR}/examples/s3_curl_ops.cpp
      ${PROJECT_SOURCE_DIR}/examples/s3_curl_ops.h
    DESTINATION share/doc/${HIPOBJ_PACKAGE_NAME}/examples
    COMPONENT devel)
  install(
    FILES ${PROJECT_SOURCE_DIR}/examples/CMakeLists.install.cmake
    DESTINATION share/doc/${HIPOBJ_PACKAGE_NAME}/examples
    RENAME CMakeLists.txt
    COMPONENT devel)
endif()

# The nightly packages were named hipobject, hipobject-dev or
# hipobject-devel, and hipobject-static-dev or hipobject-static-devel,
# without the ROCm version. A package replaces the one it was named
# before, which has the same files, so installing it removes that one.
# These can go once the nightly packages with the old names are gone.
set(HIPOBJ_OLD_RUNTIME_PACKAGE "hipobject")
set(HIPOBJ_OLD_DEB_DEVEL_PACKAGE "${HIPOBJ_DEVEL_PACKAGE_BASE}-dev")
set(HIPOBJ_OLD_RPM_DEVEL_PACKAGE "${HIPOBJ_DEVEL_PACKAGE_BASE}-devel")
if(BUILD_SHARED_LIBS)
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONFLICTS "${HIPOBJ_OLD_RUNTIME_PACKAGE}")
  set(CPACK_DEBIAN_RUNTIME_PACKAGE_REPLACES "${HIPOBJ_OLD_RUNTIME_PACKAGE}")
  set(CPACK_RPM_RUNTIME_PACKAGE_OBSOLETES "${HIPOBJ_OLD_RUNTIME_PACKAGE}")
endif()
set(CPACK_DEBIAN_DEVEL_PACKAGE_CONFLICTS "${HIPOBJ_OLD_DEB_DEVEL_PACKAGE}")
set(CPACK_DEBIAN_DEVEL_PACKAGE_REPLACES "${HIPOBJ_OLD_DEB_DEVEL_PACKAGE}")
set(CPACK_RPM_DEVEL_PACKAGE_OBSOLETES "${HIPOBJ_OLD_RPM_DEVEL_PACKAGE}")

# Create the packages. A static build only has a devel package, since
# there's no shared library for the runtime package.
rocm_create_package(
  NAME "hipObject${HIPOBJ_ROCM_MM}"
  DESCRIPTION "The hipObject library for ROCm ${HIPOBJ_ROCM_MM}"
  MAINTAINER "hipobject-maintainer@amd.com"
)
