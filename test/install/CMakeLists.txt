# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install hipObject into a scratch prefix and build a C-only consumer
# against the installed package (see run-install-test.cmake). This
# catches problems with the exported targets, package config, install
# components, and run paths, none of which the in-tree tests use.

# The consumer must use the same sanitizer as an instrumented libhipobj,
# or it can't link against the sanitizer runtime the library needs.
# Clang only links a sanitizer's C++ runtime (e.g., UBSan's vptr checks)
# when the C++ driver links, so the C consumer has to ask for it, along
# with the libstdc++ that runtime depends on.
set(HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS "")
set(HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS "")
if(HIPOBJ_USE_SANITIZERS)
  set(HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS
    "-fsanitize=${HIPOBJ_SANITIZER_TYPE}")
  set(HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS
    "${HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS}")
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    string(APPEND HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS
      " -fsanitize-link-c++-runtime -lstdc++")
  endif()
endif()

# An instrumented static library needs the code coverage profile runtime
# (see HIPOBJCoverage.cmake). The shared library links it itself.
if(HIPOBJ_USE_CODE_COVERAGE AND NOT BUILD_SHARED_LIBS)
  string(APPEND HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS
    " -fprofile-instr-generate")
endif()

# The consumer finds hip and hsa-runtime64 (which the static library's
# package config looks for) where this build found them
set(HIPOBJ_INSTALL_TEST_PREFIX_PATH ${CMAKE_PREFIX_PATH})
foreach(package IN ITEMS hip hsa-runtime64)
  if(${package}_DIR)
    # <prefix>/lib/cmake/<package>
    get_filename_component(package_prefix "${${package}_DIR}/../../.."
      ABSOLUTE)
    list(APPEND HIPOBJ_INSTALL_TEST_PREFIX_PATH ${package_prefix})
  endif()
endforeach()
list(REMOVE_DUPLICATES HIPOBJ_INSTALL_TEST_PREFIX_PATH)

set(HIPOBJ_INSTALL_TEST_SETTINGS
  ${CMAKE_CURRENT_BINARY_DIR}/install-test-settings.cmake)
file(CONFIGURE
  OUTPUT ${HIPOBJ_INSTALL_TEST_SETTINGS}
  CONTENT [[
set(BUILD_DIR "@PROJECT_BINARY_DIR@")
set(TEST_DIR "@CMAKE_CURRENT_BINARY_DIR@/install-test")
set(CONSUMER_SOURCE_DIR "@CMAKE_CURRENT_SOURCE_DIR@/consumer")
set(GENERATOR "@CMAKE_GENERATOR@")
set(MAKE_PROGRAM "@CMAKE_MAKE_PROGRAM@")
set(C_COMPILER "@CMAKE_C_COMPILER@")
set(CONSUMER_FLAGS "@HIPOBJ_INSTALL_TEST_CONSUMER_FLAGS@")
set(CONSUMER_LINK_FLAGS "@HIPOBJ_INSTALL_TEST_CONSUMER_LINK_FLAGS@")
set(CONSUMER_PREFIX_PATH "@HIPOBJ_INSTALL_TEST_PREFIX_PATH@")
set(EXPECTED_VERSION "@HIPOBJ_LIBRARY_VERSION@")
]]
  @ONLY)

add_test(
  NAME hipobj-install
  COMMAND ${CMAKE_COMMAND}
    -DSETTINGS=${HIPOBJ_INSTALL_TEST_SETTINGS}
    -DCONFIG=$<CONFIG>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run-install-test.cmake)

# Build the DEB and RPM packages and check their names, contents,
# dependencies, and versions (see run-package-test.cmake). Each test is
# skipped if hipObject isn't configured to build its packages, which
# needs dpkg or rpmbuild, or the tools to read them aren't installed.
#
# The package version includes ROCM_LIBPATCH_VERSION, which
# HIPOBJInstall.cmake sets from ROCM_VERSION if the environment doesn't
if(DEFINED ENV{ROCM_LIBPATCH_VERSION})
  set(HIPOBJ_PACKAGE_TEST_LIBPATCH "$ENV{ROCM_LIBPATCH_VERSION}")
else()
  string(REPLACE "." "0" HIPOBJ_PACKAGE_TEST_LIBPATCH "${ROCM_VERSION}")
endif()

# The directories added after this one declare the system libraries the
# runtime package depends on (see hipobj_add_runtime_library_dependency()),
# so write those once the whole project is configured
function(hipobj_write_package_test_dependencies file)
  get_property(deb_depends GLOBAL PROPERTY HIPOBJ_RUNTIME_DEB_DEPENDS)
  get_property(rpm_depends GLOBAL PROPERTY HIPOBJ_RUNTIME_RPM_DEPENDS)
  file(CONFIGURE
    OUTPUT ${file}
    CONTENT [[
set(RUNTIME_DEB_DEPENDS "@deb_depends@")
set(RUNTIME_RPM_DEPENDS "@rpm_depends@")
]]
    @ONLY)
endfunction()

set(HIPOBJ_PACKAGE_TEST_DEPENDENCIES
  ${CMAKE_CURRENT_BINARY_DIR}/package-test-dependencies.cmake)
# A deferred call's arguments are evaluated when it runs, in hipObject's
# top-level directory, so use EVAL to pass the file name's value now
cmake_language(EVAL CODE "
  cmake_language(DEFER DIRECTORY [[${PROJECT_SOURCE_DIR}]]
    CALL hipobj_write_package_test_dependencies
      [[${HIPOBJ_PACKAGE_TEST_DEPENDENCIES}]])")

# CPack always writes its configuration to the top of the build tree,
# even when hipObject is a subproject
set(HIPOBJ_PACKAGE_TEST_SETTINGS
  ${CMAKE_CURRENT_BINARY_DIR}/package-test-settings.cmake)
file(CONFIGURE
  OUTPUT ${HIPOBJ_PACKAGE_TEST_SETTINGS}
  CONTENT [[
set(BUILD_DIR "@PROJECT_BINARY_DIR@")
set(DEPENDENCIES "@HIPOBJ_PACKAGE_TEST_DEPENDENCIES@")
set(TEST_DIR "@CMAKE_CURRENT_BINARY_DIR@/package-test")
set(CPACK_CONFIG "@CMAKE_BINARY_DIR@/CPackConfig.cmake")
set(INSTALL_PREFIX "@CMAKE_INSTALL_PREFIX@")
set(SHARED "@BUILD_SHARED_LIBS@")
set(LIBRARY_VERSION "@HIPOBJ_LIBRARY_VERSION@")
set(LIBRARY_SOVERSION "@HIPOBJ_LIBRARY_SOVERSION@")
set(ROCM_VERSION_MAJOR "@HIPOBJ_ROCM_VERSION_MAJOR@")
set(ROCM_VERSION_MINOR "@HIPOBJ_ROCM_VERSION_MINOR@")
set(ROCM_LIBPATCH_VERSION "@HIPOBJ_PACKAGE_TEST_LIBPATCH@")
]]
  @ONLY)

foreach(generator IN ITEMS DEB RPM)
  string(TOLOWER ${generator} generator_lower)
  add_test(
    NAME hipobj-package-${generator_lower}
    COMMAND ${CMAKE_COMMAND}
      -DSETTINGS=${HIPOBJ_PACKAGE_TEST_SETTINGS}
      -DGENERATOR=${generator}
      -DCONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run-package-test.cmake)
  # CPack builds the preinstall target in the build tree, so don't let
  # the package tests run it at the same time
  set_tests_properties(hipobj-package-${generator_lower} PROPERTIES
    SKIP_REGULAR_EXPRESSION "run-package-test: SKIPPED"
    RESOURCE_LOCK hipobj-cpack)
endforeach()
