/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Default run-time options for the sanitizers
 *
 * HIPOBJSanitizers.cmake adds this file to every program in a sanitizer
 * build. Each sanitizer's runtime calls its function, if the program
 * defines it, when the program starts. The options in the ASAN_OPTIONS,
 * UBSAN_OPTIONS, and TSAN_OPTIONS environment variables are applied
 * after these, so they can override them.
 *
 * The functions have to be visible to the runtime, which looks them up
 * by name when it's a shared library.
 *
 * The runtimes choose these functions' names, which are reserved
 * identifiers. clang-tidy's check for reserved identifiers, which goes
 * by three names, is turned off here for that reason.
 */

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */

#define HIPOBJ_SANITIZER_DEFAULT_OPTIONS __attribute__((used, visibility("default")))

/* AddressSanitizer
 *
 *   - detect_invalid_pointer_pairs=2 turns on the pointer-compare and
 *     pointer-subtract checks, including for null pointers.
 *   - detect_stack_use_after_return=1 finds uses of a function's local
 *     variables after it returns.
 *   - check_initialization_order=1 and strict_init_order=1 find global
 *     initializers that use globals from other files before those are
 *     initialized.
 *   - strict_string_checks=1 checks that the strings passed to the C
 *     library's string functions are terminated.
 */
HIPOBJ_SANITIZER_DEFAULT_OPTIONS const char *__asan_default_options(void);
const char *
__asan_default_options(void)
{
    return "detect_invalid_pointer_pairs=2:"
           "detect_stack_use_after_return=1:"
           "check_initialization_order=1:"
           "strict_init_order=1:"
           "strict_string_checks=1";
}

/* UndefinedBehaviorSanitizer
 *
 *   - print_stacktrace=1 shows where each report came from.
 */
HIPOBJ_SANITIZER_DEFAULT_OPTIONS const char *__ubsan_default_options(void);
const char *
__ubsan_default_options(void)
{
    return "print_stacktrace=1";
}

/* ThreadSanitizer
 *
 *   - second_deadlock_stack=1 shows where both locks in a possible
 *     deadlock were taken, not just one.
 */
HIPOBJ_SANITIZER_DEFAULT_OPTIONS const char *__tsan_default_options(void);
const char *
__tsan_default_options(void)
{
    return "second_deadlock_stack=1";
}

/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
