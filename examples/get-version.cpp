/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Example: get hipObject's version, and check that the library the
 *          program is running with can be used with the header it was
 *          compiled with.
 *
 * Usage:
 *   get-version
 */

#include <cstdio>
#include <cstdlib>

#include "hipobj.h"

int
main()
{
    // The version of the header this program was compiled with. These
    // are macros, so #if can test them too.
    printf("Compiled with hipObject %d.%d.%d\n", HIPOBJ_VERSION_MAJOR, HIPOBJ_VERSION_MINOR,
           HIPOBJ_VERSION_PATCH);

    // The version of the library it's running with, as text. These
    // functions don't need hipObjInit().
    printf("Running with hipObject %s\n", hipObjGetVersionString());

    // The same version, as numbers to compare. A null pointer skips that
    // part, here the patch version, which the check below doesn't need.
    unsigned            major = 0;
    unsigned            minor = 0;
    const hipObjError_t err   = hipObjGetVersion(&major, &minor, nullptr);
    if (err.opError != hipObjSuccess) {
        fprintf(stderr, "hipObjGetVersion failed: %s\n", hipObjGetErrorString(err.opError));
        return EXIT_FAILURE;
    }

    // A library with the header's major version has the header's ABI, and
    // later minor versions only add to it, so the library's minor version
    // must be at least the header's. While the major version is 0,
    // hipObject is in development, and any minor version can change the
    // ABI, so the minor versions must match.
    const unsigned headerMajor = HIPOBJ_VERSION_MAJOR;
    const unsigned headerMinor = HIPOBJ_VERSION_MINOR;
    const bool     compatible =
        major == headerMajor && (major == 0 ? minor == headerMinor : minor >= headerMinor);
    if (!compatible) {
        fprintf(stderr, "The library (%u.%u) can't be used with the header (%u.%u)\n", major, minor,
                headerMajor, headerMinor);
        return EXIT_FAILURE;
    }
    printf("The library can be used with the header\n");
    return EXIT_SUCCESS;
}
