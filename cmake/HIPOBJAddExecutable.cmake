# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

include(GNUInstallDirs)
include(HIPOBJClangTidy)
include(HIPOBJCompilerOptions)
include(HIPOBJIWYU)

# Add an executable program using hipObject build conventions
#
# Parameters:
#   NAME <name>                     The name of the executable program to create
#   SRCS [src1 [src2 ...]]          The source files
#   SYSINCLS [path1 [path2 ...]]    Third-party include dirs (added as
#                                   SYSTEM so their warnings are suppressed)
#
# NOTE: Other include directories, link libraries, and compile
#       definitions are still set by the caller.
function(hipobj_add_executable)

  # Parse arguments
  set(options) # None at this time
  set(oneValueArgs NAME)
  set(multiValueArgs SRCS SYSINCLS)
  cmake_parse_arguments(PARSE_ARGV 0 arg
    "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(NOT arg_NAME)
    message(FATAL_ERROR "hipobj_add_executable: NAME is required")
  endif()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "hipobj_add_executable: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()

  add_executable(${arg_NAME} ${arg_SRCS})

  # Set the C++ standard and position-independent code
  set_target_properties(${arg_NAME} PROPERTIES
    CXX_STANDARD ${HIPOBJ_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

  if(arg_SYSINCLS)
    target_include_directories(${arg_NAME} SYSTEM PRIVATE ${arg_SYSINCLS})
  endif()

  hipobj_set_compiler_flags(${arg_NAME})
  hipobj_set_linker_flags(${arg_NAME})
  hipobj_set_clang_tidy(${arg_NAME})
  hipobj_set_iwyu(${arg_NAME})
endfunction()

# Add an executable test program using hipObject build conventions
#
# Parameters: Same as hipobj_add_executable()
#
# NOTE: Adds -UNDEBUG so test programs always have assert() available,
#       even in release builds.
function(hipobj_add_test_executable)
  hipobj_add_executable(${ARGN})

  # Only NAME is needed here; hipobj_add_executable() validated the rest
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME" "")
  target_compile_options(${arg_NAME} PRIVATE -UNDEBUG)
endfunction()

# Install a program that links hipobj
#
# Parameters:
#   NAME <name>             The executable target to install
#   DESTINATION <dir>       The install directory, relative to the
#                           install prefix
#
# Gives the installed program a run path, so it runs without
# LD_LIBRARY_PATH:
#   - CMAKE_INSTALL_LIBDIR, relative to the program ($ORIGIN), finds
#     libhipobj wherever the install prefix is
#   - INSTALL_RPATH_USE_LINK_PATH adds the directory the ROCm libraries
#     were linked from, as for the installed library (see
#     HIPOBJInstall.cmake)
#
# The program goes in the runtime component, so it's in the runtime
# package. A static build has no runtime package.
function(hipobj_install_executable)

  # Parse arguments
  set(options) # None at this time
  set(oneValueArgs NAME DESTINATION)
  set(multiValueArgs) # None at this time
  cmake_parse_arguments(PARSE_ARGV 0 arg
    "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(NOT arg_NAME)
    message(FATAL_ERROR "hipobj_install_executable: NAME is required")
  endif()
  if(NOT arg_DESTINATION OR IS_ABSOLUTE "${arg_DESTINATION}")
    message(FATAL_ERROR
      "hipobj_install_executable: DESTINATION must be a relative path")
  endif()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "hipobj_install_executable: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
  endif()

  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(libdir "${CMAKE_INSTALL_LIBDIR}")
  else()
    # Both directories are relative to the install prefix, so root them
    # at / just to get the path from one to the other
    file(RELATIVE_PATH libdir
      "/${arg_DESTINATION}" "/${CMAKE_INSTALL_LIBDIR}")
    set(libdir "\$ORIGIN/${libdir}")
  endif()

  set_target_properties(${arg_NAME} PROPERTIES
    INSTALL_RPATH "${libdir}"
    INSTALL_RPATH_USE_LINK_PATH ON)

  install(TARGETS ${arg_NAME}
    RUNTIME DESTINATION ${arg_DESTINATION}
    COMPONENT runtime)
endfunction()

# Make the runtime package depend on shared libraries that an installed
# program links
#
# Parameters:
#   SONAMES <soname> [<soname> ...]  The libraries' sonames
#                                    (e.g., libcurl.so.4), which the RPM
#                                    package requires
#   DEB <dependency>                 The Debian package that provides
#                                    them (e.g., "libcurl4t64 | libcurl4")
#
# The RPM packages don't generate their requirements automatically (see
# HIPOBJInstall.cmake), so they require the sonames, which every
# distribution's library packages provide.
#
# HIPOBJInstall.cmake adds the dependencies to the runtime package,
# which is only created for a shared build.
function(hipobj_add_runtime_library_dependency)

  # Parse arguments
  set(options) # None at this time
  set(oneValueArgs DEB)
  set(multiValueArgs SONAMES)
  cmake_parse_arguments(PARSE_ARGV 0 arg
    "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(NOT arg_SONAMES OR NOT arg_DEB)
    message(FATAL_ERROR
      "hipobj_add_runtime_library_dependency: SONAMES and DEB are required")
  endif()
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "hipobj_add_runtime_library_dependency: unknown arguments: "
      "${arg_UNPARSED_ARGUMENTS}")
  endif()

  set_property(GLOBAL APPEND PROPERTY
    HIPOBJ_RUNTIME_DEB_DEPENDS "${arg_DEB}")

  # RPM marks the sonames of 64-bit libraries with "()(64bit)"
  foreach(soname IN LISTS arg_SONAMES)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(rpm_depend "${soname}()(64bit)")
    else()
      set(rpm_depend "${soname}")
    endif()
    set_property(GLOBAL APPEND PROPERTY
      HIPOBJ_RUNTIME_RPM_DEPENDS "${rpm_depend}")
  endforeach()
endfunction()
