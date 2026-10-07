# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Check that a shared library exports only the public C API
#
# Usage:
#   cmake -DNM=<nm> -DLIBRARY=<path to libhipobj.so> -P check-exports.cmake
#
# Fails if any symbol in the library's dynamic symbol table is anything
# other than a hipObj* function (nm type T). C++ symbols are mangled
# (_Z...), so a leaked hipObj:: internal or C++ standard library
# instantiation doesn't match. Also fails if no hipObj* functions are
# found at all, so a change in nm's output can't make the check pass
# vacuously.

foreach(var IN ITEMS NM LIBRARY)
  if(NOT ${var})
    message(FATAL_ERROR "check-exports: ${var} is not set")
  endif()
endforeach()

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

set(api_count 0)
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

  if(type STREQUAL "T" AND name MATCHES "^hipObj[A-Za-z0-9_]*$")
    math(EXPR api_count "${api_count} + 1")
  else()
    list(APPEND leaked "  ${type} ${name}")
  endif()
endforeach()

if(api_count EQUAL 0)
  message(FATAL_ERROR
    "check-exports: no hipObj* functions are exported from ${LIBRARY}")
endif()

if(leaked)
  list(LENGTH leaked leaked_count)
  list(JOIN leaked "\n" leaked_text)
  message(FATAL_ERROR
    "check-exports: ${LIBRARY} exports ${leaked_count} symbol(s) that "
    "aren't part of the public C API:\n${leaked_text}\n"
    "Only functions declared with HIPOBJ_API in include/hipobj.h may be "
    "exported (see src/hipobj.map).")
endif()

message(STATUS
  "check-exports: ${LIBRARY} exports only the public C API "
  "(${api_count} functions)")
