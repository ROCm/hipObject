# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Check that the shared library exports only the public C API. A static
# library has no dynamic symbol table, so there's nothing to check.

get_target_property(HIPOBJ_LIBRARY_TYPE hipobj TYPE)
if(HIPOBJ_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
  if(NOT CMAKE_NM)
    message(FATAL_ERROR "nm wasn't found, so the hipobj-exports test can't run")
  endif()

  add_test(
    NAME hipobj-exports
    COMMAND ${CMAKE_COMMAND}
      -DNM=${CMAKE_NM}
      -DLIBRARY=$<TARGET_FILE:hipobj>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check-exports.cmake)
endif()
