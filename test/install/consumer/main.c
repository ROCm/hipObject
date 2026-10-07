/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Calls into the installed library and checks that the version it
 * reports matches the version of the installed CMake package
 */

#include <hipobj.h>

#include <stdio.h>
#include <string.h>

int
main(void)
{
    const char *version = hipObjGetVersionString();

    if (strcmp(version, HIPOBJ_EXPECTED_VERSION) != 0) {
        fprintf(stderr, "hipObjGetVersionString() returned %s, expected %s\n", version,
                HIPOBJ_EXPECTED_VERSION);
        return 1;
    }

    printf("hipObject %s\n", version);
    return 0;
}
