/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Warning suppression macros for hipObject
 * Modeled after hipFile's hipfile-warnings.h
 */

#pragma once

#define HIPOBJ_WARN_JOINSTR(x, y) x y
#define HIPOBJ_WARN_DO_PRAGMA(x) _Pragma(#x)
#define HIPOBJ_WARN_PRAGMA(x) HIPOBJ_WARN_DO_PRAGMA(GCC diagnostic x)

/* clang-format off */
#if (defined(__GNUC__) || defined(__clang__))
    #define HIPOBJ_WARN_OFF(x) HIPOBJ_WARN_PRAGMA(push) HIPOBJ_WARN_PRAGMA(ignored HIPOBJ_WARN_JOINSTR("-W", x))
    #define HIPOBJ_WARN_ON(x)  HIPOBJ_WARN_PRAGMA(pop)
#endif
/* clang-format on */

#if defined(__clang__) || defined(__GNUC__)
#define HIPOBJ_WARN_FORMAT_NONLITERAL_OFF HIPOBJ_WARN_OFF("format-nonliteral")
#define HIPOBJ_WARN_FORMAT_NONLITERAL_ON HIPOBJ_WARN_ON("format-nonliteral")
#else
#define HIPOBJ_WARN_FORMAT_NONLITERAL_OFF
#define HIPOBJ_WARN_FORMAT_NONLITERAL_ON
#endif

#if defined(__clang__)
#define HIPOBJ_WARN_NO_EXIT_DTOR_OFF HIPOBJ_WARN_OFF("exit-time-destructors")
#define HIPOBJ_WARN_NO_EXIT_DTOR_ON HIPOBJ_WARN_ON("exit-time-destructors")
#else
#define HIPOBJ_WARN_NO_EXIT_DTOR_OFF
#define HIPOBJ_WARN_NO_EXIT_DTOR_ON
#endif

#if defined(__clang__)
#define HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF HIPOBJ_WARN_OFF("global-constructors")
#define HIPOBJ_WARN_NO_GLOBAL_CTOR_ON HIPOBJ_WARN_ON("global-constructors")
#else
#define HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF
#define HIPOBJ_WARN_NO_GLOBAL_CTOR_ON
#endif
