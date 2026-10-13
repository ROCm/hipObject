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
    -pedantic

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
    -Warray-bounds=2 # The highest level
    -Wattribute-alias=2
    # Plain -Wcast-align only warns on targets that require aligned
    # access, so it does nothing on x86-64. =strict warns everywhere.
    -Wcast-align
    -Wcast-align=strict
    -Wcast-qual
    -Wcatch-value=3
    -Wconditionally-supported
    -Wconversion
    -Wctor-dtor-privacy
    -Wdate-time
    -Wdeprecated-copy-dtor
    -Wdouble-promotion
    -Wduplicated-branches
    -Wduplicated-cond
    #-Weffc++ # TODO: Fix issues
    -Wextra-semi
    -Wfloat-equal
    # Includes -Wformat-nonliteral, -Wformat-security, and -Wformat-y2k
    -Wformat=2
    -Wformat-overflow=2
    -Wformat-signedness
    -Wformat-truncation=2
    -Wimplicit-fallthrough=5
    -Winvalid-pch
    # This is a warning for when using <C++11
    #-Wlong-long
    -Wlogical-op
    -Wmissing-braces
    -Wmissing-declarations
    -Wmissing-include-dirs
    -Wnoexcept
    -Wnon-virtual-dtor
    -Wnormalized
    -Wnull-dereference
    -Wold-style-cast
    -Woverlength-strings
    -Wpacked
    #-Wpadded # Probably should be a developer warning
    -Wplacement-new=2
    -Wpointer-arith
    -Wredundant-decls
    -Wshadow # Includes -Wshadow=local
    -Wshift-overflow=2
    # In C++, -Wconversion doesn't include this
    -Wsign-conversion
    -Wsign-promo
    # Warn about functions the stack protector doesn't cover
    -Wstack-protector
    -Wno-strict-overflow
    -Wstrict-null-sentinel
    -Wstringop-overflow=4
    -Wsuggest-final-methods
    -Wsuggest-final-types
    -Wsuggest-override
    -Wswitch-default
    -Wswitch-enum
    -Wtrampolines
    -Wundef
    -Wunused-const-variable=1
    -Wunused-macros
    -Wuseless-cast
    -Wvariadic-macros
    -Wvla
    -Wzero-as-null-pointer-constant

    # TODO: Add size warnings when we pick a limit
  )

  if(compiler_version VERSION_GREATER_EQUAL 10)
    set(flags
      # Misc warnings
      -Warith-conversion
      -Wmismatched-tags
      #-Wredundant-tags # TODO: Fix issues
      ${flags}
    )
  endif()

  if(compiler_version VERSION_GREATER_EQUAL 11)
    set(flags
      # Zero the registers a function used when it returns, so their
      # contents don't outlive the call or serve as ROP gadgets
      -fzero-call-used-regs=used-gpr
      # Misc warnings
      -Wctad-maybe-unsupported
      ${flags}
    )
  endif()

  if(compiler_version VERSION_GREATER_EQUAL 12)
    set(flags
      # Misc warnings
      -Wbidi-chars=any
      -Winterference-size
      -Wtrivial-auto-var-init
      -Wuse-after-free=3
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
      -Woverloaded-virtual=2
      # Needs -fstrict-flex-arrays
      -Wstrict-flex-arrays
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

  # TODO: Turn these on when they can be tested with g++ 16
  #if(compiler_version VERSION_GREATER_EQUAL 16)
  #  set(flags
  #    # Misc warnings
  #    -Wkeyword-macro
  #    -Wsfinae-incomplete=2
  #    ${flags}
  #  )
  #endif()

  # Control-flow protection (Intel CET) only exists on x86
  check_cxx_compiler_flag(-fcf-protection=full
    HIPOBJ_COMPILER_SUPPORTS_CF_PROTECTION)
  if(HIPOBJ_COMPILER_SUPPORTS_CF_PROTECTION)
    list(APPEND flags -fcf-protection=full)
  endif()

  set(${outvar} ${flags} PARENT_SCOPE)

endfunction()
