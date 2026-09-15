# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Shim config for the in-tree fetch: minio-cpp looks nlohmann_json up
# with find_package(CONFIG REQUIRED); the fetched build directory
# carries a working config, but this shim keeps the lookup local and
# deterministic alongside the other bridge dependencies.

include_guard(GLOBAL)

if(NOT TARGET nlohmann_json::nlohmann_json)
  include(${CMAKE_CURRENT_LIST_DIR}/../HIPOBJMinioCppDeps.cmake)
endif()

set(nlohmann_json_FOUND TRUE)
