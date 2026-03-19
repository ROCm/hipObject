# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional clang-tidy integration.

option(HIPOBJ_USE_CLANG_TIDY
  "Run clang-tidy during compilation" OFF)

if(HIPOBJ_USE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE
    NAMES clang-tidy
    PATHS ${ROCM_PATH}/llvm/bin)

  if(CLANG_TIDY_EXE)
    set(CMAKE_CXX_CLANG_TIDY
      ${CLANG_TIDY_EXE}
      --extra-arg=-Wno-unknown-warning-option)
    message(STATUS
      "clang-tidy enabled: ${CLANG_TIDY_EXE}")
  else()
    message(WARNING
      "clang-tidy requested but not found")
  endif()
endif()
