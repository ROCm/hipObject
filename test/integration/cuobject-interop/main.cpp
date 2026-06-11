/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Optional probe binary for a system-installed libcuobjserver.
 * Verifies headers and linkage when HIPOBJ_FIND_CUOBJECT_SERVER=ON.
 */

#include <cstdio>

#include <cuobjserver.h>

int main() {
  fprintf(stdout,
          "hipobj-cuobject-probe: libcuobjserver linked successfully\n");
  fprintf(stdout,
          "Use MinIO AIStor + RC adapter for end-to-end cuObject interop.\n");
  return 0;
}
