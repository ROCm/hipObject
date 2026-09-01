#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the pre-built hipobj-rdma-test-server inside the ernic container.
# The binary is built on the runner (ROCm container) and mounted read-only
# at /hipobject-build.  The ernic container provides the rocm-ernic ibverbs
# device; no compilation happens here.

set -euo pipefail

export LD_LIBRARY_PATH=/hipobject-build/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

rocm-ernic --backend loopback &
sleep 1

exec /hipobject-build/test/integration/rdma-test-server/hipobj-rdma-test-server \
    9000 --v2
