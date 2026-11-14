# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# A dependency provider like the one TheRock sets in the projects it
# builds (cmake/therock_subproject_dep_provider.cmake in TheRock), for
# the hipobj-dependency-provider test. CMAKE_PROJECT_TOP_LEVEL_INCLUDES
# runs this file in hipObject's first project() call. It reads these
# cache entries, which CMakeLists.txt in this directory writes:
#   HIPOBJ_TEST_THEROCK_PACKAGES    The packages TheRock builds
#   HIPOBJ_TEST_PROVIDED_PACKAGES   The ones TheRock provides to
#                                   hipObject, which it would declare
#                                   as dependencies
#   HIPOBJ_TEST_PACKAGE_DIR_<name>  The directory of a provided
#                                   package's package files
#   HIPOBJ_TEST_SYSTEM_PACKAGES     The packages from the system
#   HIPOBJ_TEST_PROVIDER_LOG        The file the provider lists each
#                                   find_package() call's package in
#
# Like TheRock's provider, it finds a provided package only in its
# directory, and fails a find_package() for one of TheRock's packages
# that isn't provided. Unlike TheRock's, it also fails one for a
# package that's in neither list, so that each new package is checked.

# As TheRock does, so that find_package() can't find a package through
# a package registry
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)

file(WRITE "${HIPOBJ_TEST_PROVIDER_LOG}" "")

# Set out_var to the arguments of a find_package() call for a provided
# package, rewritten as TheRock's provider does: without the options
# that say where to search, and with options to search only the
# package's directory
function(hipobj_test_provided_args out_var package_name package_dir)
  if(NOT package_dir)
    message(FATAL_ERROR "No directory for the provided package ${package_name}")
  endif()

  # Every find_package() keyword. An argument after HINTS or PATHS is a
  # directory, until the next keyword.
  set(keywords
    EXACT QUIET REQUIRED COMPONENTS OPTIONAL_COMPONENTS CONFIG NO_MODULE
    MODULE GLOBAL NO_POLICY_SCOPE BYPASS_PROVIDER NAMES CONFIGS HINTS
    PATHS REGISTRY_VIEW PATH_SUFFIXES NO_DEFAULT_PATH NO_PACKAGE_ROOT_PATH
    NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_SYSTEM_ENVIRONMENT_PATH
    NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_BUILDS_PATH NO_CMAKE_SYSTEM_PATH
    NO_CMAKE_INSTALL_PREFIX NO_CMAKE_SYSTEM_PACKAGE_REGISTRY
    CMAKE_FIND_ROOT_PATH_BOTH ONLY_CMAKE_FIND_ROOT_PATH
    NO_CMAKE_FIND_ROOT_PATH)
  # The options that say where to search
  set(search_options
    BYPASS_PROVIDER NO_DEFAULT_PATH NO_PACKAGE_ROOT_PATH NO_CMAKE_PATH
    NO_CMAKE_ENVIRONMENT_PATH NO_SYSTEM_ENVIRONMENT_PATH
    NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_BUILDS_PATH NO_CMAKE_SYSTEM_PATH
    NO_CMAKE_INSTALL_PREFIX NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)

  set(args)
  set(in_directories FALSE)
  foreach(arg IN LISTS ARGN)
    if(arg IN_LIST keywords)
      set(in_directories FALSE)
    endif()
    if(in_directories)
      continue()
    endif()
    if(arg STREQUAL "HINTS" OR arg STREQUAL "PATHS")
      set(in_directories TRUE)
    elseif(arg STREQUAL "MODULE")
      # TheRock only supports this for old projects
      message(FATAL_ERROR
        "find_package(${package_name}) uses MODULE, but TheRock provides "
        "its packages as package files, not find modules")
    elseif(NOT arg IN_LIST search_options)
      list(APPEND args "${arg}")
    endif()
  endforeach()
  list(APPEND args BYPASS_PROVIDER NO_DEFAULT_PATH PATHS "${package_dir}")
  set(${out_var} "${args}" PARENT_SCOPE)
endfunction()

# A macro, not a function, so that find_package() sets its variables in
# the scope of the call it handles
macro(hipobj_test_dependency_provider method package_name)
  if("${package_name}" IN_LIST HIPOBJ_TEST_PROVIDED_PACKAGES)
    file(APPEND "${HIPOBJ_TEST_PROVIDER_LOG}" "provided ${package_name}\n")
    hipobj_test_provided_args(_hipobj_test_provided_args "${package_name}"
      "${HIPOBJ_TEST_PACKAGE_DIR_${package_name}}" ${ARGN})
    find_package(${package_name} ${_hipobj_test_provided_args})
  elseif("${package_name}" IN_LIST HIPOBJ_TEST_THEROCK_PACKAGES)
    message(FATAL_ERROR
      "find_package(${package_name}) looks for a package that TheRock "
      "builds, but that this build didn't find, so the test doesn't "
      "provide it. In TheRock, hipObject would have to declare the "
      "project that provides it as a dependency.")
  elseif("${package_name}" IN_LIST HIPOBJ_TEST_SYSTEM_PACKAGES)
    file(APPEND "${HIPOBJ_TEST_PROVIDER_LOG}" "system ${package_name}\n")
    find_package(${package_name} ${ARGN} BYPASS_PROVIDER)
  else()
    message(FATAL_ERROR
      "find_package(${package_name}) looks for a package that "
      "test/dependency-provider/CMakeLists.txt doesn't list. Add it to "
      "therock_packages if TheRock builds it, and hipObject will have to "
      "declare the project that provides it as a dependency in TheRock. "
      "Otherwise, add it to system_packages.")
  endif()
endmacro()

cmake_language(SET_DEPENDENCY_PROVIDER hipobj_test_dependency_provider
  SUPPORTED_METHODS FIND_PACKAGE)
