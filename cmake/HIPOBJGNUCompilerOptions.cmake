# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Warning flags for GNU g++
#
# https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html

include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(HIPOBJFortifySource)

function(hipobj_get_gnu_warning_flags outvar compiler_version)

  # Warning flags for g++ 9 and earlier
  set(flags
    # Basic "high" warning levels
    -Wall
    -Wextra

    # Avoid non-standard C/C++ behavior
    # Can't use -pedantic with nvcc at this time, as nvcc
    # generates code with non-standard #line styles, leading
    # to a LOT of warnings
    #-pedantic

    # Check for ABI warnings
    # Most of this is noise, but probably useful to turn
    # on from time to time
    #-Wabi

    # Turn on stack protection options
    -fstack-clash-protection
    -fstack-protector-strong

    # Keep null pointer checks the optimizer could prove redundant, and
    # don't assume that pointers to different types never alias
    -fno-delete-null-pointer-checks
    -fno-strict-aliasing

    # Check the preconditions of libstdc++ calls (bounds, etc.)
    -D_GLIBCXX_ASSERTIONS

    # Misc warnings
    #-Waggregate-return # We return structs, but might be useful
                        # to turn on to see where this can be
                        # minimized
    -Walloca
    -Walloc-zero
    -Warray-bounds=2 # TODO: Consider making =3 in newer g++
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wdate-time
    -Wdouble-promotion
    -Wduplicated-branches
    -Wduplicated-cond
    -Wfloat-equal
    -Wformat=2
    -Wformat-nonliteral
    -Wformat-overflow=2
    -Wformat-security
    -Wformat-signedness
    -Wformat-truncation=2
    -Wformat-y2k
    -Winvalid-pch
    # This is a warning for when using <C++11
    #-Wlong-long
    -Wlogical-op
    -Wmissing-declarations
    -Wnormalized
    -Wnull-dereference
    -Wpacked
    #-Wpadded # Probably should be a developer warning
    -Wpointer-arith
    -Wredundant-decls
    -Wshadow
    -Wshadow-local
    -Wshift-overflow=2
    -Wno-strict-overflow
    -Wswitch-default
    -Wswitch-enum
    -Wtrampolines
    -Wundef
    -Wuninitialized
    -Wunknown-pragmas
    -Wunsafe-loop-optimizations
    -Wunused
    -Wunused-macros
    -Wuseless-cast
    -Wvla
    -Wzero-as-null-pointer-constant

    # TODO: Add size warnings when we pick a limit
  )

  if(compiler_version VERSION_GREATER_EQUAL 11)
    set(flags
      # Zero the registers a function used when it returns, so their
      # contents don't outlive the call or serve as ROP gadgets
      -fzero-call-used-regs=used-gpr
      ${flags}
    )
  endif()

  if(compiler_version VERSION_GREATER_EQUAL 12)
    set(flags
      # Misc warnings
      -Wbidi-chars=any
      -Winterference-size
      -Wtrivial-auto-var-init
      ${flags}
    )

    # Only use _FORTIFY_SOURCE if the optimization level is -O2, -O3, or -Os
    hipobj_get_fortify_flags(fortify_flags)
    set(flags
      ${fortify_flags}
      ${flags}
    )
  endif()

  if(compiler_version VERSION_GREATER_EQUAL 13)
    set(flags
      # Turn on strict flex arrays (helps ASAN, _FORTIFY_SOURCE, etc.)
      -fstrict-flex-arrays=3
      # Misc warnings
      -Winvalid-utf8
      ${flags}
    )
  endif()

  if(compiler_version VERSION_GREATER_EQUAL 14)
    set(flags
      # Misc warnings
      -Walloc-size
      -Wcalloc-transposed-args
      -Wflex-array-member-not-at-end
      -Wnrvo
      ${flags}
    )
  endif()

  if(compiler_version VERSION_GREATER_EQUAL 15)
    set(flags
      # Zero the padding bits in every initializer of an automatic
      # variable, so stale stack memory can't leak through padding
      # (Clang has no equivalent)
      -fzero-init-padding-bits=all
      # Misc warnings
      -Wtrailing-whitespace
      # The kind names the only whitespace allowed, so =spaces flags
      # tab indentation
      -Wleading-whitespace=spaces
      ${flags}
    )
  endif()

  # Control-flow protection (Intel CET) only exists on x86
  check_cxx_compiler_flag(-fcf-protection=full
    HIPOBJ_COMPILER_SUPPORTS_CF_PROTECTION)
  if(HIPOBJ_COMPILER_SUPPORTS_CF_PROTECTION)
    list(APPEND flags -fcf-protection=full)
  endif()

  set(${outvar} ${flags} PARENT_SCOPE)

endfunction()
