# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Shim config for the in-tree fetch: minio-cpp looks pugixml up with
# find_package(CONFIG REQUIRED); the fetched build directory carries the
# config file but no exported targets, so this shim makes the lookup
# resolve to the targets HIPOBJMinioCppDeps.cmake already created.

include_guard(GLOBAL)

if(NOT TARGET pugixml::pugixml)
  include(${CMAKE_CURRENT_LIST_DIR}/../HIPOBJMinioCppDeps.cmake)
endif()

set(pugixml_FOUND TRUE)
