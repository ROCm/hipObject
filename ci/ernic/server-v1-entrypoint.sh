#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the pre-built hipobj-rdma-test-server (v1 mode) inside the ernic
# container.  Binary is built on the runner and mounted at /hipobject-build.

set -euo pipefail

export LD_LIBRARY_PATH=/hipobject-build/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
exec /hipobject-build/test/integration/rdma-test-server/hipobj-rdma-test-server \
    9000
