#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Build hipobj-rdma-test-server inside the ernic image and run it.
# rocm-ernic is already installed in the image; it registers itself
# as an ibverbs provider so ibv_get_device_list() sees it.

set -euo pipefail

BUILD_DIR=/hipobject-build-server

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
