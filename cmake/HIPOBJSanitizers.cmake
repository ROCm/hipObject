# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sanitizer support
#
# There are two options:
#
#   - HIPOBJ_USE_SANITIZERS enables every sanitizer, except
#     ThreadSanitizer, that the compiler supports and that can be combined
#     with AddressSanitizer. The lists for GCC and Clang are below.
#   - HIPOBJ_USE_THREAD_SANITIZER enables ThreadSanitizer.
#
# ThreadSanitizer can't be combined with AddressSanitizer, so only one of
# the options can be on. Both options work with GCC and with Clang. The C
# and C++ compilers must be the same kind, since a program can only
# contain one sanitizer runtime.
#
# HIPOBJ_SANITIZER_SHARED_RUNTIME selects how programs get the sanitizer
# runtime. When it's on, which is the default, they link the runtime's
# shared library. Libraries from another sanitizer build that uses the
# shared runtime, such as a sanitizer build of ROCm, can then be loaded
# into the same program. When it's off, programs link the static runtime
# instead. (Without an option, Clang links the static runtime and GCC
# links the shared one.)
#
# A sanitizer build instruments every target that hipObject builds,
# including third-party code that it builds from source, such as
# GoogleTest. This module adds the sanitizer flags to the targets itself,
# at the end of the configuration, so the code that creates the targets
# doesn't need to know about the sanitizers. Libraries that come from the
# system or from ROCm aren't instrumented.
#
# The options are checked, and the compilers are tested with the
# sanitizer flags, when this module is included, so include it early in
# the top-level CMakeLists.txt.

include_guard(GLOBAL)

include(CheckSourceCompiles)

option(HIPOBJ_USE_SANITIZERS
  "Build with every sanitizer except ThreadSanitizer"
  OFF)
option(HIPOBJ_USE_THREAD_SANITIZER
  "Build with ThreadSanitizer (not with HIPOBJ_USE_SANITIZERS)"
  OFF)
option(HIPOBJ_SANITIZER_SHARED_RUNTIME
  "Link the shared sanitizer runtime instead of the static one"
  ON)

# Store the flags that compile and link hipObject's code with the enabled
# sanitizer in compile_var and link_var. Both are empty lists if neither
# sanitizer option is on.
function(hipobj_get_sanitizer_flags compile_var link_var)
  get_property(compile_options GLOBAL
    PROPERTY HIPOBJ_SANITIZER_COMPILE_OPTIONS)
  get_property(link_options GLOBAL
    PROPERTY HIPOBJ_SANITIZER_LINK_OPTIONS)
  set(${compile_var} ${compile_options} PARENT_SCOPE)
  set(${link_var} ${link_options} PARENT_SCOPE)
endfunction()

# Store the flags for a program that's built outside this build, and
# links an instrumented hipObject, in compile_var and link_var. The
# program can be C or C++. Both are empty lists if neither sanitizer
# option is on.
function(hipobj_get_sanitizer_consumer_flags compile_var link_var)
  hipobj_get_sanitizer_flags(compile_options link_options)
  get_property(consumer_link_options GLOBAL
    PROPERTY HIPOBJ_SANITIZER_CONSUMER_LINK_OPTIONS)
  set(${compile_var} ${compile_options} PARENT_SCOPE)
  set(${link_var} ${link_options} ${consumer_link_options} PARENT_SCOPE)
endfunction()

# Add the sanitizer flags to every target in a directory and in the
# directories below it. A deferred call runs this on hipObject's
# top-level directory at the end of the configuration (see below).
function(hipobj_add_sanitizers_to_directory dir)
  hipobj_get_sanitizer_flags(compile_options link_options)

  get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(NOT type MATCHES "^(EXECUTABLE|(STATIC|SHARED|MODULE|OBJECT)_LIBRARY)$")
      continue()
    endif()

    target_compile_options(${target} PRIVATE ${compile_options})
    target_link_options(${target} PRIVATE ${link_options})

    # Every program gets the default run-time options
    if(type STREQUAL "EXECUTABLE")
      target_sources(${target} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sanitizer-default-options.c)
    endif()

    # A shared library can't link the static runtime. Its references to
    # the runtime stay undefined, and the program that loads it provides
    # them. This comes after any -z defs (--no-undefined) that the
    # library was given, since the target's other options were all added
    # before this deferred call ran, and the last one wins.
    if(type MATCHES "^(SHARED|MODULE)_LIBRARY$"
        AND NOT HIPOBJ_SANITIZER_SHARED_RUNTIME)
      target_link_options(${target} PRIVATE "LINKER:-z,undefs")
    endif()
  endforeach()

  get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    hipobj_add_sanitizers_to_directory(${subdir})
  endforeach()
endfunction()

# Find the directory that holds a sanitizer's shared runtime library, and
# store it in out_var if programs have to be told where it is (an RPATH).
# out_var is empty if the dynamic linker already searches the directory.
function(hipobj_find_sanitizer_runtime_dir out_var runtime)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang's runtimes are named for the architecture, except when it's
    # built with a separate runtime directory for each target
    set(names
      libclang_rt.${runtime}-${CMAKE_SYSTEM_PROCESSOR}.so
      libclang_rt.${runtime}.so)
  else()
    set(names lib${runtime}.so)
  endif()

  # The compiler prints the name it was given if it can't find the file.
  # It's given the user's flags, since some, such as -resource-dir,
  # change where it looks.
  separate_arguments(flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
  foreach(name IN LISTS names)
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} ${flags} -print-file-name=${name}
      OUTPUT_VARIABLE path
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE result)
    if(result EQUAL 0 AND IS_ABSOLUTE "${path}" AND EXISTS "${path}")
      break()
    endif()
    set(path)
  endforeach()
  if(NOT path)
    list(JOIN names ", " names)
    message(FATAL_ERROR
      "Can't find the shared runtime library for the sanitizers. "
      "${CMAKE_CXX_COMPILER} can't find any of these: ${names}. Install "
      "the compiler's sanitizer runtimes, or set "
      "HIPOBJ_SANITIZER_SHARED_RUNTIME to OFF.")
  endif()

  # GCC's libasan.so, for example, is a link to the libasan.so.8 in a
  # system library directory, which the dynamic linker searches already
  file(REAL_PATH "${path}" path)
  cmake_path(GET path PARENT_PATH dir)
  set(${out_var} PARENT_SCOPE)
  foreach(implicit_dir IN LISTS CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES)
    file(REAL_PATH "${implicit_dir}" implicit_dir)
    if(dir STREQUAL implicit_dir)
      return()
    endif()
  endforeach()
  set(${out_var} ${dir} PARENT_SCOPE)
endfunction()

# HIPOBJ_SANITIZER_TYPE was removed when HIPOBJ_USE_THREAD_SANITIZER
# replaced it. Configuring fails if it's still set, so that an old build
# directory that used it to select ThreadSanitizer doesn't silently switch
# to AddressSanitizer.
if(DEFINED HIPOBJ_SANITIZER_TYPE)
  message(FATAL_ERROR
    "HIPOBJ_SANITIZER_TYPE has been removed. Use HIPOBJ_USE_SANITIZERS "
    "for AddressSanitizer and UndefinedBehaviorSanitizer, or "
    "HIPOBJ_USE_THREAD_SANITIZER for ThreadSanitizer, and remove "
    "HIPOBJ_SANITIZER_TYPE from the cache (-U HIPOBJ_SANITIZER_TYPE).")
endif()

if(HIPOBJ_USE_SANITIZERS AND HIPOBJ_USE_THREAD_SANITIZER)
  message(FATAL_ERROR
    "HIPOBJ_USE_SANITIZERS and HIPOBJ_USE_THREAD_SANITIZER can't both be "
    "on, since ThreadSanitizer can't be combined with AddressSanitizer")
endif()

if(NOT (HIPOBJ_USE_SANITIZERS OR HIPOBJ_USE_THREAD_SANITIZER))
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$"
    OR NOT CMAKE_C_COMPILER_ID STREQUAL CMAKE_CXX_COMPILER_ID)
  message(FATAL_ERROR
    "The sanitizers need GCC or Clang as both the C and the C++ "
    "compiler, but the C compiler is ${CMAKE_C_COMPILER_ID} and the C++ "
    "compiler is ${CMAKE_CXX_COMPILER_ID}")
endif()

# The -fsanitize groups to enable, and the name of the runtime library
# that implements them
#
# For HIPOBJ_USE_SANITIZERS, both compilers get:
#
#   - address: AddressSanitizer. On Linux, this includes LeakSanitizer.
#   - undefined: UndefinedBehaviorSanitizer's default checks.
#   - float-divide-by-zero: floating-point division by zero, which
#     "undefined" leaves out.
#   - pointer-compare and pointer-subtract: AddressSanitizer checks for
#     comparing or subtracting pointers into different objects. They only
#     run when the run-time option detect_invalid_pointer_pairs is set,
#     which sanitizer-default-options.c does. With Clang,
#     sanitizer-ignorelist.txt turns them off in the C++ standard
#     library's comparison function objects, such as std::less, which
#     compare such pointers on purpose.
#
# Clang also gets these checks, which its "undefined" leaves out:
#
#   - implicit-conversion: implicit conversions that change a value.
#     These are defined behavior, but they're usually bugs.
#     sanitizer-ignorelist.txt turns the checks off in the C++ standard
#     library headers, which make such conversions on purpose.
#   - local-bounds: out-of-bounds accesses that can be found from local
#     information alone.
#   - nullability: violations of the _Nonnull annotations.
#
# Clang's "integer" group isn't used. "implicit-conversion" includes its
# conversion checks, and "undefined" includes its checks for signed
# overflow, shifts, and division by zero. Its remaining checks are for
# unsigned overflow, which is defined behavior that code such as hash
# functions depends on.
#
# GCC also gets these checks, which its "undefined" leaves out:
#
#   - bounds-strict: the "bounds" check, extended to arrays at the end
#     of a struct.
#   - float-cast-overflow: conversions from floating point to an integer
#     type that can't hold the value. Clang's "undefined" includes it.
if(HIPOBJ_USE_THREAD_SANITIZER)
  set(hipobj_sanitizer_checks thread)
  set(hipobj_sanitizer_runtime tsan)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(hipobj_sanitizer_checks
    address
    undefined
    float-divide-by-zero
    pointer-compare
    pointer-subtract
    implicit-conversion
    local-bounds
    nullability)
  set(hipobj_sanitizer_runtime asan)
else()
  set(hipobj_sanitizer_checks
    address
    undefined
    float-divide-by-zero
    pointer-compare
    pointer-subtract
    bounds-strict
    float-cast-overflow)
  set(hipobj_sanitizer_runtime asan)
endif()
list(JOIN hipobj_sanitizer_checks "," hipobj_sanitizer_checks)

# -fno-sanitize-recover=all makes every report fatal, so a test with a
# report fails. Without it, UndefinedBehaviorSanitizer prints the report
# and the program keeps running.
set(hipobj_sanitizer_options
  -fsanitize=${hipobj_sanitizer_checks}
  -fno-sanitize-recover=all)
if(HIPOBJ_USE_SANITIZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(APPEND hipobj_sanitizer_options
    -fsanitize-ignorelist=${CMAKE_CURRENT_LIST_DIR}/sanitizer-ignorelist.txt)
endif()

set(hipobj_sanitizer_compile_options
  ${hipobj_sanitizer_options}
  -fno-omit-frame-pointer)
set(hipobj_sanitizer_link_options ${hipobj_sanitizer_options})

# Select the shared or the static runtime. A program that uses the shared
# runtime has to find it when it runs, so add its directory to the
# program's RPATH if the dynamic linker doesn't search it already.
#
# Clang only links the C++ part of a sanitizer's runtime when its C++
# driver links the program, so a C program that links an instrumented
# hipObject has to ask for it, along with the libstdc++ that it needs.
# GCC's runtimes don't have a separate C++ part.
set(hipobj_sanitizer_consumer_link_options)
if(HIPOBJ_SANITIZER_SHARED_RUNTIME)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(APPEND hipobj_sanitizer_link_options -shared-libsan)
  endif()
  hipobj_find_sanitizer_runtime_dir(hipobj_sanitizer_runtime_dir
    ${hipobj_sanitizer_runtime})
  if(hipobj_sanitizer_runtime_dir)
    list(APPEND hipobj_sanitizer_link_options
      "-Wl,-rpath,${hipobj_sanitizer_runtime_dir}")
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(APPEND hipobj_sanitizer_link_options -static-libsan)
elseif(HIPOBJ_USE_THREAD_SANITIZER)
  list(APPEND hipobj_sanitizer_link_options -static-libtsan)
else()
  list(APPEND hipobj_sanitizer_link_options
    -static-libasan
    -static-libubsan)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(APPEND hipobj_sanitizer_consumer_link_options
    -fsanitize-link-c++-runtime
    -lstdc++)
endif()

# Check that both compilers can build and link a program with the flags.
# This fails if, for example, the sanitizer runtimes aren't installed.
# The result is cached under a name made from the flags, so it's checked
# again if they change.
string(MD5 hipobj_sanitizer_flags_hash
  "${hipobj_sanitizer_compile_options};${hipobj_sanitizer_link_options}")
list(JOIN hipobj_sanitizer_compile_options " " CMAKE_REQUIRED_FLAGS)
set(CMAKE_REQUIRED_LINK_OPTIONS ${hipobj_sanitizer_link_options})
list(JOIN hipobj_sanitizer_link_options " " hipobj_sanitizer_link_flags)
set(CMAKE_REQUIRED_QUIET ON)
foreach(lang IN ITEMS C CXX)
  set(result HIPOBJ_SANITIZERS_WORK_${lang}_${hipobj_sanitizer_flags_hash})
  check_source_compiles(${lang} "int main(void) { return 0; }" ${result})
  if(NOT ${result})
    message(FATAL_ERROR
      "The ${lang} compiler, ${CMAKE_${lang}_COMPILER}, can't build a "
      "program with the sanitizers. Check that its sanitizer runtimes "
      "are installed. The flags were:\n"
      "  Compile: ${CMAKE_REQUIRED_FLAGS}\n"
      "  Link: ${hipobj_sanitizer_link_flags}")
  endif()
endforeach()
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
unset(CMAKE_REQUIRED_QUIET)

set_property(GLOBAL PROPERTY HIPOBJ_SANITIZER_COMPILE_OPTIONS
  ${hipobj_sanitizer_compile_options})
set_property(GLOBAL PROPERTY HIPOBJ_SANITIZER_LINK_OPTIONS
  ${hipobj_sanitizer_link_options})
set_property(GLOBAL PROPERTY HIPOBJ_SANITIZER_CONSUMER_LINK_OPTIONS
  ${hipobj_sanitizer_consumer_link_options})

message(STATUS "Sanitizers: ${hipobj_sanitizer_checks}")

# Instrument every target once they've all been created. The arguments
# are evaluated now, so the call gets hipObject's top-level directory
# even if hipObject is part of another project.
cmake_language(EVAL CODE "
  cmake_language(DEFER DIRECTORY [[${PROJECT_SOURCE_DIR}]]
    CALL hipobj_add_sanitizers_to_directory [[${PROJECT_SOURCE_DIR}]])")
