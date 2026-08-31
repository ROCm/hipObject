#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Build the minio-cpp bridge and run minio-getput-rdma in host-buffer
# (nogpu) mode against hipobj-rdma-test-server in v1 protocol mode.
# rocm-ernic provides the emulated verbs device so actual RDMA data
# movement happens; the example verifies payload integrity end-to-end.
#
# Environment variables (all have defaults):
#   SERVER_ENDPOINT  - http URL of hipobj-rdma-test-server (default: http://ernic-server:9000)
#   TEST_SIZE        - transfer size in bytes              (default: 65536)
#   BUILD_DIR        - cmake build tree                    (default: /hipobject-build-minio-v1)

set -euo pipefail

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://ernic-server:9000}"
TEST_SIZE="${TEST_SIZE:-65536}"
BUILD_DIR="${BUILD_DIR:-/hipobject-build-minio-v1}"

SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

cmake \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -S /hipobject \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DHIPOBJ_IONIC=ON \
    -DHIPOBJ_BNXT=OFF \
    -DHIPOBJ_INTEGRATION_TESTS=OFF \
    -DHIPOBJ_MINIO_CLIENT=ON \
    -DHIPOBJ_BUILD_DOCS=OFF

cmake --build "${BUILD_DIR}" \
      --target minio-getput-rdma \
      --parallel "$(nproc)"

# Unit tests that require no NIC.
echo "--- unit tests ---"
cmake --build "${BUILD_DIR}" --target test-rdma-token --parallel "$(nproc)"
"${BUILD_DIR}/test/unit/test-rdma-token"

# minio-getput-rdma in nogpu (host-buffer) mode: allocates a
# page-aligned host buffer, PUTs it via the minio-cpp RDMA bridge, GETs
# it back, and memcmp-verifies every byte.  rocm-ernic emulates the NIC
# so this is a real RDMA transfer, not a 501 fallback.
echo "--- minio-cpp bridge v1 PUT + GET (payload verification) ---"
out=""
rc=0
out=$("${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" \
    "${SERVER_HOST}:${SERVER_PORT}" \
    minioadmin minioadmin \
    "${TEST_SIZE}" nogpu 2>&1) || rc=$?
echo "${out}"

echo "${out}" | grep -q "Data integrity check passed" || {
    echo "ERROR: minio-cpp bridge v1 payload verification failed (exit ${rc})"
    exit 1
}

echo "--- ernic minio-v1 integration: PASS ---"
