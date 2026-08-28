#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Build hipobject examples inside the ernic image and exercise
# put-object / get-object against hipobj-rdma-test-server.
#
# Environment variables (all have defaults):
#   SERVER_ENDPOINT  - http URL of hipobj-rdma-test-server (default: http://ernic-server:9000)
#   TEST_SIZE        - object size in bytes              (default: 1048576)
#   BUILD_DIR        - where to put the cmake build tree (default: /hipobject-build-client)

set -euo pipefail

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://ernic-server:9000}"
TEST_SIZE="${TEST_SIZE:-1048576}"
BUILD_DIR="${BUILD_DIR:-/hipobject-build-client}"
BUCKET=hipobj-ci
OBJECT=ernic-test-object

cmake \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -S /hipobject \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DHIPOBJ_IONIC=ON \
    -DHIPOBJ_BNXT=OFF \
    -DHIPOBJ_INTEGRATION_TESTS=OFF \
    -DHIPOBJ_MINIO_CLIENT=OFF \
    -DHIPOBJ_BUILD_DOCS=OFF

cmake --build "${BUILD_DIR}" \
      --target put-object get-object \
      --parallel "$(nproc)"

# Layer 1: unit tests (no NIC needed)
echo "--- unit tests ---"
ctest --test-dir "${BUILD_DIR}" \
      -R 'test-rdma-token|test-rdma-header' \
      --output-on-failure

# Layer 2: RDMA PUT against hipobj-rdma-test-server
#
# With the v2 protocol a complete PUT ends with "PUT ok" logged by the example.
# Accepting "would send RDMA token" without "PUT ok" is no longer a pass — that
# only means the client reached the token stage but the transfer did not complete.
echo "--- RDMA PUT ---"
put_out=""
put_rc=0
put_out=$("${BUILD_DIR}/hipobj-examples/put-object" \
    "${TEST_SIZE}" \
    --live "${SERVER_ENDPOINT}" \
    "${BUCKET}" "${OBJECT}" 2>&1) || put_rc=$?
echo "${put_out}"

echo "${put_out}" | grep -q "PUT ok\|succeeded" || {
    echo "ERROR: PUT did not complete successfully (exit ${put_rc})"
    echo "Output: ${put_out}"
    exit 1
}

# Layer 2: RDMA GET — seed object is already present from the PUT above.
echo "--- RDMA GET ---"
get_out=""
get_rc=0
get_out=$("${BUILD_DIR}/hipobj-examples/get-object" \
    "${TEST_SIZE}" \
    --live "${SERVER_ENDPOINT}" \
    "${BUCKET}" "${OBJECT}" 2>&1) || get_rc=$?
echo "${get_out}"

echo "${get_out}" | grep -q "GET ok\|succeeded\|Data integrity" || {
    echo "ERROR: GET did not complete successfully (exit ${get_rc})"
    echo "Output: ${get_out}"
    exit 1
}

echo "--- ernic integration: PASS ---"
