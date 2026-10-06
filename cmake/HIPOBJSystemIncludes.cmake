# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Treat third-party headers as system headers

include_guard(GLOBAL)

# Mark the usage-requirement include directories of every library target
# created under dir (recursively) as SYSTEM, so warnings in third-party
# headers don't fire in hipObject targets that consume them.
#
# Use this on the source directory of a FetchContent_MakeAvailable()
# dependency. CMake 3.25 can do this with FetchContent_Declare(SYSTEM),
# but our minimum is 3.21. Imported targets (find_package) are already
# treated as SYSTEM by CMake and don't need this.
function(hipobj_mark_dir_targets_system dir)
  get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(NOT type MATCHES "LIBRARY$")
      continue()
    endif()
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
      set_property(TARGET ${target} APPEND
        PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${dirs})
    endif()
  endforeach()

  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    hipobj_mark_dir_targets_system("${subdir}")
  endforeach()
endfunction()
