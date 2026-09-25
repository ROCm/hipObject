#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the pre-built hipobj-rdma-test-server inside an ernic guest VM, in v1
# mode: the server answers S3 PUT/GET carrying x-amz-rdma-token and drives
# the transfer over the verbs device. The v2 control protocol has its own
# coverage in the unit tests and the v2-control-e2e ctest; this lane is here
# for the v1 wire, which is what the minio-cpp bridge speaks.
# The binary and its ROCm closure are built on the runner (ROCm container)
# and copied into the guest at ${BUILD_DIR}; no compilation happens here.
#
# The verbs device comes from the guest's ionic/ionic_rdma drivers bound to
# the emulated PCI function that rocm-ernic serves on the host. Nothing
# ernic-side runs in here.
#
# Environment variables (all have defaults):
#   BUILD_DIR    - where the binaries were copied (default: /tmp/hipobject-build)
#   SERVER_PORT  - control-plane listen port      (default: 9000)

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/tmp/hipobject-build}"
export LD_LIBRARY_PATH="${BUILD_DIR}/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec "${BUILD_DIR}/test/integration/rdma-test-server/hipobj-rdma-test-server" \
    "${SERVER_PORT:-9000}"
