# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build hipObject's DEB or RPM packages and check them
#
# Usage:
#   cmake -DSETTINGS=<settings.cmake> -DGENERATOR=<DEB|RPM> [-DCONFIG=<config>]
#     -P run-package-test.cmake
#
# SETTINGS is written by CMakeLists.txt in this directory. CONFIG is the
# build configuration to package, for multi-config generators. Prints
# "run-package-test: SKIPPED" and succeeds if hipObject isn't configured
# to build GENERATOR packages or the tools to read them aren't
# installed.
#
# Checks:
#   - The package names, which end with the ROCm major and minor version
#     (<mm>): hipobject<mm> and hipobject-dev<mm> (DEB) or
#     hipobject-devel<mm> (RPM), or a single hipobject-static-dev<mm>
#     (DEB) or hipobject-static-devel<mm> (RPM) for a static build
#   - That each package replaces the one it was named before, without
#     the ROCm version: Conflicts and Replaces (DEB) or Obsoletes (RPM)
#   - That the devel package depends on the runtime package from the same
#     build, with its exact version and release
#   - The files each package contains, including the examples' sources
#     in the devel package and the tools in the runtime package, that no
#     file is in two packages, and that the test programs and the
#     example programs aren't packaged, nor the tools in a static build
#   - The package dependencies, which depend on the ROCm version and on
#     the system libraries the installed programs link
#   - That the package holding the library, and only that one,
#     recommends libibverbs (except for RPM packages built with a CMake
#     before 4.1, which can't)
#   - That the package version includes the ROCm version
#   - For RPM, that the runtime package provides the library's soname
#     and that the packages don't own ROCm's directories

# A script has no policy settings otherwise, and IN_LIST needs CMP0057
cmake_minimum_required(VERSION 3.25.2)

if(NOT SETTINGS)
  message(FATAL_ERROR "run-package-test: SETTINGS is not set")
endif()
if(NOT GENERATOR MATCHES "^(DEB|RPM)$")
  message(FATAL_ERROR
    "run-package-test: GENERATOR must be DEB or RPM, not '${GENERATOR}'")
endif()
include(${SETTINGS})

# Fail the test with a message. The message can't contain semicolons,
# so lists in it have to be joined first.
function(fail)
  string(JOIN "" text ${ARGN})
  message(FATAL_ERROR "run-package-test: ${text}")
endfunction()

# Run a command, fail with its output if it fails, and set out_var to
# its standard output
function(run_command out_var description)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    fail("${description} failed:\n${output}${error}")
  endif()
  set(${out_var} "${output}" PARENT_SCOPE)
endfunction()

# Set out_var to the generators the build is configured for. Including
# CPackConfig.cmake in a function keeps the rest of its variables out
# of the script.
function(get_cpack_generators out_var)
  include(${CPACK_CONFIG})
  set(${out_var} "${CPACK_GENERATOR}" PARENT_SCOPE)
endfunction()

# rocm_create_package() only sets up the generators whose tools it
# finds (dpkg or rpmbuild) when hipObject is configured
get_cpack_generators(cpack_generators)
if(NOT GENERATOR IN_LIST cpack_generators)
  message("run-package-test: SKIPPED: hipObject isn't configured to "
    "build ${GENERATOR} packages")
  return()
endif()

if(GENERATOR STREQUAL "DEB")
  find_program(DPKG_DEB dpkg-deb)
  if(NOT DPKG_DEB)
    message("run-package-test: SKIPPED: dpkg-deb isn't installed")
    return()
  endif()
  set(package_extension deb)
  set(devel_suffix -dev)
else()
  find_program(RPM rpm)
  if(NOT RPM)
    message("run-package-test: SKIPPED: rpm isn't installed")
    return()
  endif()
  set(package_extension rpm)
  set(devel_suffix -devel)
endif()

set(package_dir ${TEST_DIR}/${GENERATOR})
set(tests_prefix ${TEST_DIR}/tests-prefix)

set(config_args)
if(CONFIG)
  set(config_args -C ${CONFIG})
endif()

file(REMOVE_RECURSE ${TEST_DIR})
file(MAKE_DIRECTORY ${TEST_DIR})

run_command(unused "cpack -G ${GENERATOR}"
  ${CMAKE_CPACK_COMMAND} --config ${CPACK_CONFIG} -G ${GENERATOR}
    -B ${package_dir} ${config_args})

# Install the tests component, which isn't packaged, to get the paths of
# the installed test programs
set(install_config_args)
if(CONFIG)
  set(install_config_args --config ${CONFIG})
endif()
run_command(unused "install the tests component"
  ${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${tests_prefix}
    --component tests ${install_config_args})
file(GLOB_RECURSE test_files
  LIST_DIRECTORIES false
  RELATIVE ${tests_prefix}
  ${tests_prefix}/*)

# What each package must contain, relative to the install prefix
set(cmake_package_files
  lib/cmake/hipobj/hipobj-config.cmake
  lib/cmake/hipobj/hipobj-config-version.cmake
  lib/cmake/hipobj/hipobj-targets.cmake)
set(rocm_mm "${ROCM_VERSION_MAJOR}.${ROCM_VERSION_MINOR}")
set(license_file share/doc/hipobject${rocm_mm}/LICENSE.md)

# The examples' sources, which the devel package has when they're
# installed. The example programs themselves aren't installed.
set(example_files)
if(EXAMPLES_DIR)
  foreach(file IN ITEMS CMakeLists.txt README.md get-version.cpp
      get-object.cpp put-object.cpp s3_curl_ops.cpp s3_curl_ops.h)
    list(APPEND example_files ${EXAMPLES_DIR}/${file})
  endforeach()
endif()
set(not_packaged_files
  bin/hipobj-examples/get-version
  bin/hipobj-examples/get-object
  bin/hipobj-examples/put-object)

# The tools, which the runtime package has when they're installed. A
# static build installs them, but has no runtime package to put them in.
set(tool_files)
if(TOOLS)
  set(tool_files bin/hipobj-test-install)
endif()

# The ROCm packages the library's package depends on
set(rocm_depends "amdrocm-runtime${devel_suffix}${rocm_mm}")
set(rocm_not_depends rocm-core)

# The system libraries the installed programs link, which the runtime
# package depends on. A DEB dependency can list alternatives
# ("a | b"), and each of them has to be listed.
include(${DEPENDENCIES})
if(GENERATOR STREQUAL "DEB")
  set(program_depends)
  foreach(depend IN LISTS RUNTIME_DEB_DEPENDS)
    string(REGEX REPLACE "[ \t]" "" depend "${depend}")
    string(REPLACE "|" ";" depend "${depend}")
    list(APPEND program_depends ${depend})
  endforeach()
else()
  set(program_depends ${RUNTIME_RPM_DEPENDS})
endif()

# What the package holding the library recommends. Empty for an RPM
# package built with a CMake before 4.1 (see HIPOBJInstall.cmake).
if(GENERATOR STREQUAL "DEB")
  set(library_recommends ${LIBRARY_DEB_RECOMMENDS})
else()
  set(library_recommends ${LIBRARY_RPM_RECOMMENDS})
endif()

# <name>_replaces is the package's name before it had the ROCm version
if(SHARED)
  set(runtime_package hipobject${rocm_mm})
  set(devel_package hipobject${devel_suffix}${rocm_mm})
  set(expected_packages ${runtime_package} ${devel_package})

  set(${runtime_package}_files
    lib/libhipobj.so.${LIBRARY_SOVERSION}
    lib/libhipobj.so.${LIBRARY_VERSION}
    ${license_file}
    ${tool_files})
  set(${runtime_package}_depends ${rocm_depends} ${program_depends})
  set(${runtime_package}_not_depends ${rocm_not_depends})
  set(${runtime_package}_recommends ${library_recommends})
  set(${runtime_package}_not_recommends)
  set(${runtime_package}_replaces hipobject)

  set(${devel_package}_files
    include/hipobj/hipobj.h
    lib/libhipobj.so
    ${cmake_package_files}
    ${example_files})
  set(${devel_package}_depends ${runtime_package})
  set(${devel_package}_not_depends)
  set(${devel_package}_recommends)
  set(${devel_package}_not_recommends ${library_recommends})
  set(${devel_package}_replaces hipobject${devel_suffix})
else()
  set(runtime_package)
  set(devel_package hipobject-static${devel_suffix}${rocm_mm})
  set(expected_packages ${devel_package})

  set(${devel_package}_files
    include/hipobj/hipobj.h
    lib/libhipobj.a
    ${cmake_package_files}
    ${license_file}
    ${example_files})
  set(${devel_package}_depends ${rocm_depends})
  # The installed programs aren't packaged
  set(${devel_package}_not_depends ${rocm_not_depends} ${program_depends})
  set(${devel_package}_recommends ${library_recommends})
  set(${devel_package}_not_recommends)
  set(${devel_package}_replaces hipobject-static${devel_suffix})
  list(APPEND not_packaged_files ${tool_files})
endif()

# rocm_create_package() appends ROCM_LIBPATCH_VERSION to the version.
# DEB versions also have a release (-<release>).
string(REPLACE "." "\\." expected_version_regex
  "${LIBRARY_VERSION}.${ROCM_LIBPATCH_VERSION}")
if(GENERATOR STREQUAL "DEB")
  string(APPEND expected_version_regex "-.+")
endif()

# The directories that ROCm owns, which the RPM packages mustn't (see
# HIPOBJInstall.cmake)
set(rocm_dirs
  /opt
  /opt/rocm
  ${INSTALL_PREFIX}
  ${INSTALL_PREFIX}/bin
  ${INSTALL_PREFIX}/include
  ${INSTALL_PREFIX}/lib
  ${INSTALL_PREFIX}/lib/cmake
  ${INSTALL_PREFIX}/share
  ${INSTALL_PREFIX}/share/doc)

# Turn a DEB relationship field into a list of package names, without
# version constraints: "pkg (>= 1), a | b" -> "pkg;a;b"
function(parse_deb_relations out_var text)
  string(REGEX REPLACE "\\([^)]*\\)" "" text "${text}")
  string(REGEX REPLACE "[ \t\n]" "" text "${text}")
  string(REGEX REPLACE "[,|]" ";" text "${text}")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Turn rpm's list of relationships, one "<name> [<op> <version>]" per
# line, into a list of names
function(parse_rpm_relations out_var text)
  string(REGEX REPLACE " [^\n]*" "" text "${text}")
  string(STRIP "${text}" text)
  string(REPLACE "\n" ";" text "${text}")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Turn a relationship field into a list of its entries, with their
# version constraints and single spaces: "a (= 1),\n b" -> "a (= 1);b".
# DEB fields separate them with commas, and rpm lists one per line.
function(split_relations out_var text)
  if(GENERATOR STREQUAL "DEB")
    string(REPLACE "," ";" text "${text}")
  else()
    string(REPLACE "\n" ";" text "${text}")
  endif()
  set(entries)
  foreach(entry IN LISTS text)
    string(REGEX REPLACE "[ \t\n]+" " " entry "${entry}")
    string(STRIP "${entry}" entry)
    if(NOT entry STREQUAL "")
      list(APPEND entries "${entry}")
    endif()
  endforeach()
  set(${out_var} "${entries}" PARENT_SCOPE)
endfunction()

# Read a package
#
# Sets, in the caller's scope:
#   package_name         The package name
#   package_version      The package version
#   package_full_version The package version and release, as another
#                        package's dependency on this one names it
#   package_depends      The names of the packages and capabilities it
#                        depends on, without version constraints
#   package_depend_entries The same with their version constraints, as
#                        split_relations() writes them
#   package_recommends   The names of what it recommends
#   package_conflicts    The names of the packages it conflicts with
#                        (DEB) or obsoletes (RPM)
#   package_replaces     The names of the packages it replaces (DEB) or
#                        obsoletes (RPM)
#   package_provides     The capabilities it provides (RPM only)
#   package_files        The absolute paths of the files it contains
#   package_dirs         The absolute paths of the directories it
#                        contains (DEB) or owns (RPM)
function(read_package package)
  set(files)
  set(dirs)
  set(provides)

  if(GENERATOR STREQUAL "DEB")
    run_command(name "read ${package}" ${DPKG_DEB} -f ${package} Package)
    run_command(version "read ${package}" ${DPKG_DEB} -f ${package} Version)
    run_command(depends "read ${package}" ${DPKG_DEB} -f ${package} Depends)
    run_command(recommends "read ${package}"
      ${DPKG_DEB} -f ${package} Recommends)
    run_command(conflicts "read ${package}"
      ${DPKG_DEB} -f ${package} Conflicts)
    run_command(replaces "read ${package}" ${DPKG_DEB} -f ${package} Replaces)
    run_command(listing "list ${package}" ${DPKG_DEB} -c ${package})

    # A DEB version includes the release
    string(STRIP "${version}" full_version)
    split_relations(depend_entries "${depends}")
    parse_deb_relations(depends "${depends}")
    parse_deb_relations(recommends "${recommends}")
    parse_deb_relations(conflicts "${conflicts}")
    parse_deb_relations(replaces "${replaces}")

    # <mode> <owner> <size> <date> <time> ./<path>[ -> <target>]
    string(REPLACE "\n" ";" listing "${listing}")
    foreach(line IN LISTS listing)
      if(NOT line MATCHES
          "^(.)[^ ]* +[^ ]+ +[^ ]+ +[^ ]+ +[^ ]+ +\\.(/[^ ]*)")
        continue()
      endif()
      if(CMAKE_MATCH_1 STREQUAL "d")
        string(REGEX REPLACE "(.)/$" "\\1" dir "${CMAKE_MATCH_2}")
        list(APPEND dirs "${dir}")
      else()
        list(APPEND files "${CMAKE_MATCH_2}")
      endif()
    endforeach()
  else()
    run_command(name "read ${package}"
      ${RPM} -qp --queryformat "%{NAME}" ${package})
    run_command(version "read ${package}"
      ${RPM} -qp --queryformat "%{VERSION}" ${package})
    run_command(full_version "read ${package}"
      ${RPM} -qp --queryformat "%{VERSION}-%{RELEASE}" ${package})
    run_command(depends "read ${package}" ${RPM} -qp --requires ${package})
    run_command(recommends "read ${package}"
      ${RPM} -qp --recommends ${package})
    run_command(obsoletes "read ${package}"
      ${RPM} -qp --obsoletes ${package})
    run_command(provides "read ${package}" ${RPM} -qp --provides ${package})
    run_command(listing "list ${package}"
      ${RPM} -qp --queryformat "[%{FILEMODES:perms} %{FILENAMES}\n]"
        ${package})

    string(STRIP "${full_version}" full_version)
    split_relations(depend_entries "${depends}")
    parse_rpm_relations(depends "${depends}")
    parse_rpm_relations(recommends "${recommends}")
    # Obsoletes does the work of DEB's Conflicts and Replaces
    parse_rpm_relations(conflicts "${obsoletes}")
    set(replaces "${conflicts}")
    string(STRIP "${provides}" provides)
    string(REPLACE "\n" ";" provides "${provides}")

    # <mode> <path>
    string(REPLACE "\n" ";" listing "${listing}")
    foreach(line IN LISTS listing)
      if(NOT line MATCHES "^(.)[^ ]* (/.*)$")
        continue()
      endif()
      if(CMAKE_MATCH_1 STREQUAL "d")
        list(APPEND dirs "${CMAKE_MATCH_2}")
      else()
        list(APPEND files "${CMAKE_MATCH_2}")
      endif()
    endforeach()
  endif()

  string(STRIP "${name}" name)
  string(STRIP "${version}" version)
  set(package_name "${name}" PARENT_SCOPE)
  set(package_version "${version}" PARENT_SCOPE)
  set(package_full_version "${full_version}" PARENT_SCOPE)
  set(package_depends "${depends}" PARENT_SCOPE)
  set(package_depend_entries "${depend_entries}" PARENT_SCOPE)
  set(package_recommends "${recommends}" PARENT_SCOPE)
  set(package_conflicts "${conflicts}" PARENT_SCOPE)
  set(package_replaces "${replaces}" PARENT_SCOPE)
  set(package_provides "${provides}" PARENT_SCOPE)
  set(package_files "${files}" PARENT_SCOPE)
  set(package_dirs "${dirs}" PARENT_SCOPE)
endfunction()

file(GLOB packages ${package_dir}/*.${package_extension})
if(NOT packages)
  fail("cpack -G ${GENERATOR} didn't create any packages")
endif()

set(found_packages)
set(all_files)
foreach(package IN LISTS packages)
  read_package(${package})
  get_filename_component(package_file ${package} NAME)
  set(what "${package_file} (${package_name})")

  if(NOT package_name IN_LIST expected_packages)
    list(JOIN expected_packages ", " expected_text)
    fail("${what}: unexpected package (expected ${expected_text})")
  endif()
  list(APPEND found_packages ${package_name})

  if(NOT package_version MATCHES "^${expected_version_regex}$")
    fail("${what}: version ${package_version} doesn't match "
      "${expected_version_regex}")
  endif()

  list(JOIN package_depends ", " depends_text)
  foreach(depend IN LISTS ${package_name}_depends)
    if(NOT depend IN_LIST package_depends)
      fail("${what}: doesn't depend on ${depend} (depends on "
        "${depends_text})")
    endif()
  endforeach()
  foreach(depend IN LISTS ${package_name}_not_depends)
    if(depend IN_LIST package_depends)
      fail("${what}: depends on ${depend}")
    endif()
  endforeach()

  list(JOIN package_recommends ", " recommends_text)
  foreach(recommend IN LISTS ${package_name}_recommends)
    if(NOT recommend IN_LIST package_recommends)
      fail("${what}: doesn't recommend ${recommend} (recommends "
        "${recommends_text})")
    endif()
  endforeach()
  foreach(recommend IN LISTS ${package_name}_not_recommends)
    if(recommend IN_LIST package_recommends)
      fail("${what}: recommends ${recommend}")
    endif()
  endforeach()

  # Replaces its old name, so installing it removes the old package
  set(old_name "${${package_name}_replaces}")
  if(NOT old_name IN_LIST package_conflicts
      OR NOT old_name IN_LIST package_replaces)
    list(JOIN package_conflicts ", " conflicts_text)
    list(JOIN package_replaces ", " replaces_text)
    fail("${what}: doesn't replace ${old_name} (conflicts with "
      "${conflicts_text}; replaces ${replaces_text})")
  endif()

  # Kept for the checks between packages, after this loop
  set(${package_name}_full_version "${package_full_version}")
  set(${package_name}_depend_entries "${package_depend_entries}")

  # Every file is under the install prefix and is in one package
  set(relative_files)
  foreach(file IN LISTS package_files)
    string(FIND "${file}" "${INSTALL_PREFIX}/" prefix_position)
    if(NOT prefix_position EQUAL 0)
      fail("${what}: ${file} isn't under ${INSTALL_PREFIX}")
    endif()
    if(file IN_LIST all_files)
      fail("${what}: ${file} is in another package too")
    endif()
    list(APPEND all_files "${file}")
    file(RELATIVE_PATH relative_file ${INSTALL_PREFIX} ${file})
    list(APPEND relative_files ${relative_file})
  endforeach()

  foreach(file IN LISTS ${package_name}_files)
    if(NOT file IN_LIST relative_files)
      fail("${what}: doesn't contain ${INSTALL_PREFIX}/${file}")
    endif()
  endforeach()
  foreach(file IN LISTS test_files)
    if(file IN_LIST relative_files)
      fail("${what}: contains the test program ${INSTALL_PREFIX}/${file}")
    endif()
  endforeach()
  foreach(file IN LISTS not_packaged_files)
    if(file IN_LIST relative_files)
      fail("${what}: contains ${INSTALL_PREFIX}/${file}")
    endif()
  endforeach()

  if(GENERATOR STREQUAL "RPM")
    foreach(dir IN LISTS rocm_dirs)
      if(dir IN_LIST package_dirs)
        fail("${what}: owns ROCm's directory ${dir}")
      endif()
    endforeach()

    if(package_name STREQUAL runtime_package)
      set(soname_provides_regex
        "^libhipobj\\.so\\.${LIBRARY_SOVERSION}\\(\\)")
      set(found_soname OFF)
      foreach(provide IN LISTS package_provides)
        if(provide MATCHES "${soname_provides_regex}")
          set(found_soname ON)
        endif()
      endforeach()
      if(NOT found_soname)
        list(JOIN package_provides ", " provides_text)
        fail("${what}: doesn't provide libhipobj.so.${LIBRARY_SOVERSION} "
          "(provides ${provides_text})")
      endif()
    endif()
  endif()

  message(STATUS "run-package-test: ${what}: OK")
endforeach()

foreach(package_name IN LISTS expected_packages)
  if(NOT package_name IN_LIST found_packages)
    fail("cpack -G ${GENERATOR} didn't create ${package_name}")
  endif()
endforeach()
list(REMOVE_DUPLICATES found_packages)
list(LENGTH found_packages found_count)
list(LENGTH packages package_count)
if(NOT found_count EQUAL package_count)
  fail("cpack -G ${GENERATOR} created more than one package with the "
    "same name")
endif()

# The devel package depends on the runtime package from the same build,
# by its exact version and release, so it can't be installed with
# another build's library
if(SHARED)
  set(full_version "${${runtime_package}_full_version}")
  if(GENERATOR STREQUAL "DEB")
    set(exact_depend "${runtime_package} (= ${full_version})")
  else()
    set(exact_depend "${runtime_package} = ${full_version}")
  endif()
  if(NOT exact_depend IN_LIST ${devel_package}_depend_entries)
    list(JOIN ${devel_package}_depend_entries ", " depends_text)
    fail("${devel_package} doesn't depend on ${exact_depend} (depends on "
      "${depends_text})")
  endif()
endif()
