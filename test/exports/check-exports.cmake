# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Check that a shared library exports the public C API, and nothing else
#
# Usage:
#   cmake -DNM=<nm> -DLIBRARY=<path to libhipobj.so>
#     -DHEADER=<path to hipobj.h> -P check-exports.cmake
#
# Reads the functions that the public header declares with HIPOBJ_API,
# and fails if:
#   - The library's dynamic symbol table has any symbol other than a
#     function (nm type T) that the header declares. C++ symbols are
#     mangled (_Z...), so a leaked hipObj:: internal or C++ standard
#     library instantiation never matches.
#   - The library doesn't export a function that the header declares,
#     such as one that's declared but never defined. Without this
#     check, nothing would notice until a caller failed to link.
# Also fails if it finds no HIPOBJ_API declarations in the header, or a
# HIPOBJ_API that it can't read a function name from, so a change in
# the header's style can't make the check pass vacuously.

# A script has no policy settings otherwise, and IN_LIST needs CMP0057
cmake_minimum_required(VERSION 3.25.2)

foreach(var IN ITEMS NM LIBRARY HEADER)
  if(NOT ${var})
    message(FATAL_ERROR "check-exports: ${var} is not set")
  endif()
endforeach()

# Read the functions the header declares with HIPOBJ_API

file(READ "${HEADER}" header_text)

# Remove the comments, so that a mention of HIPOBJ_API in the
# documentation, or a declaration that's commented out, doesn't count.
# The header is C, so they're /* */ comments.
set(code "")
set(rest "${header_text}")
while(TRUE)
  string(FIND "${rest}" "/*" comment_start)
  if(comment_start EQUAL -1)
    string(APPEND code "${rest}")
    break()
  endif()
  string(SUBSTRING "${rest}" 0 ${comment_start} before_comment)
  # A comment separates tokens like a space does
  string(APPEND code "${before_comment} ")
  math(EXPR comment_start "${comment_start} + 2")
  string(SUBSTRING "${rest}" ${comment_start} -1 rest)

  string(FIND "${rest}" "*/" comment_end)
  if(comment_end EQUAL -1)
    message(FATAL_ERROR "check-exports: ${HEADER} has an unterminated comment")
  endif()
  math(EXPR comment_end "${comment_end} + 2")
  string(SUBSTRING "${rest}" ${comment_end} -1 rest)
endwhile()

# Each declaration is "HIPOBJ_API <return type> <name>(<parameters>);",
# where the return type can end with a "*" instead of a space, and a
# long one can break the line before the name. The characters that are
# excluded between HIPOBJ_API and the name keep a match from running
# past the end of a declaration, or from starting at the #define of
# HIPOBJ_API.
set(declaration_regex
  "HIPOBJ_API[ \t\n][^;(){}#]*[ \t\n*](hipObj[A-Za-z0-9_]*)[ \t\n]*\\(")
string(REGEX MATCHALL "${declaration_regex}" declarations "${code}")

set(declared)
foreach(declaration IN LISTS declarations)
  string(REGEX MATCH "${declaration_regex}" unused "${declaration}")
  list(APPEND declared "${CMAKE_MATCH_1}")
endforeach()

# Every HIPOBJ_API, except where it's defined, has to be part of a
# declaration that was read. The characters around it keep a longer name
# that contains it, such as HIPOBJ_API_VERSION, from counting.
string(REGEX MATCHALL "(^|[^A-Za-z0-9_])HIPOBJ_API([^A-Za-z0-9_]|$)"
  api_markers "${code}")
string(REGEX MATCHALL "#[ \t]*define[ \t]+HIPOBJ_API([^A-Za-z0-9_]|$)"
  api_defines "${code}")
list(LENGTH api_markers marker_count)
list(LENGTH api_defines define_count)
list(LENGTH declared declared_count)
math(EXPR expected_count "${marker_count} - ${define_count}")
if(NOT declared_count EQUAL expected_count)
  list(JOIN declared ", " declared_text)
  message(FATAL_ERROR
    "check-exports: ${HEADER} uses HIPOBJ_API ${expected_count} time(s) "
    "outside its #define, but only ${declared_count} of them could be "
    "read as a function declaration (found: ${declared_text})")
endif()
if(declared_count EQUAL 0)
  message(FATAL_ERROR
    "check-exports: no HIPOBJ_API functions are declared in ${HEADER}")
endif()

# A function can be declared more than once
list(REMOVE_DUPLICATES declared)

# Read the library's exports

execute_process(
  COMMAND ${NM} -D --defined-only ${LIBRARY}
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error
  RESULT_VARIABLE nm_result)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR
    "check-exports: '${NM} -D --defined-only ${LIBRARY}' failed "
    "(${nm_result}):\n${nm_error}")
endif()

string(REPLACE "\n" ";" nm_lines "${nm_output}")

set(exported)
set(leaked)
foreach(line IN LISTS nm_lines)
  if(line STREQUAL "")
    continue()
  endif()

  # Each line is "<address> <type> <name>"
  if(NOT line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) (.+)$")
    message(FATAL_ERROR "check-exports: can't parse nm output line: ${line}")
  endif()
  set(type "${CMAKE_MATCH_1}")
  set(name "${CMAKE_MATCH_2}")

  if(type STREQUAL "T" AND name IN_LIST declared)
    list(APPEND exported "${name}")
  else()
    list(APPEND leaked "  ${type} ${name}")
  endif()
endforeach()

set(missing)
foreach(name IN LISTS declared)
  if(NOT name IN_LIST exported)
    list(APPEND missing "  ${name}")
  endif()
endforeach()

# Report both problems before failing, since one change can cause both

set(failed FALSE)
if(leaked)
  list(LENGTH leaked leaked_count)
  list(JOIN leaked "\n" leaked_text)
  message(SEND_ERROR
    "check-exports: ${LIBRARY} exports ${leaked_count} symbol(s) that "
    "aren't part of the public C API:\n${leaked_text}\n"
    "Only functions declared with HIPOBJ_API in ${HEADER} may be "
    "exported (see src/hipobj.map).")
  set(failed TRUE)
endif()

if(missing)
  list(LENGTH missing missing_count)
  list(JOIN missing "\n" missing_text)
  message(SEND_ERROR
    "check-exports: ${LIBRARY} doesn't export ${missing_count} "
    "function(s) declared with HIPOBJ_API in ${HEADER}:\n"
    "${missing_text}\n"
    "Define each one in the library, or remove its declaration.")
  set(failed TRUE)
endif()

if(failed)
  message(FATAL_ERROR "check-exports: the exports don't match the public C API")
endif()

list(LENGTH exported exported_count)
message(STATUS
  "check-exports: ${LIBRARY} exports the public C API, and nothing else "
  "(${exported_count} functions)")
