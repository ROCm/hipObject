#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Build hipobj-rdma-test-server and run it in v1 (single-round-trip) mode.
# Used by the ernic-minio-v1 CI job to provide an RDMA-capable S3 backend
# for the minio-cpp bridge integration test.

set -euo pipefail

BUILD_DIR=/hipobject-build-server-v1

cmake \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -S /hipobject \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DHIPOBJ_IONIC=ON \
    -DHIPOBJ_BNXT=OFF \
    -DHIPOBJ_INTEGRATION_TESTS=ON \
    -DHIPOBJ_MINIO_CLIENT=OFF \
    -DHIPOBJ_BUILD_DOCS=OFF

cmake --build "${BUILD_DIR}" \
      --target hipobj-rdma-test-server \
      --parallel "$(nproc)"

exec "${BUILD_DIR}/test/integration/rdma-test-server/hipobj-rdma-test-server" 9000
