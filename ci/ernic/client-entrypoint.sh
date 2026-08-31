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
      --target put-object get-object v2-data-client \
      --parallel "$(nproc)"

# Layer 1: unit tests (no NIC needed)
echo "--- unit tests ---"
ctest --test-dir "${BUILD_DIR}" \
      -R 'test-rdma-token|test-rdma-header' \
      --output-on-failure

# Layer 2: RDMA PUT against hipobj-rdma-test-server
#
# Command substitution under set -e: if the binary exits non-zero,
# bash aborts before we can print output or grep for diagnostics.
# Capture exit status separately with ||: so -e does not fire.
echo "--- RDMA PUT ---"
put_out=""
put_rc=0
put_out=$("${BUILD_DIR}/hipobj-examples/put-object" \
    "${TEST_SIZE}" \
    --live "${SERVER_ENDPOINT}" \
    "${BUCKET}" "${OBJECT}" 2>&1) || put_rc=$?
echo "${put_out}"

# The token line proves the RC QP came up; a non-zero exit without it
# indicates a failure before RDMA was attempted.
echo "${put_out}" | grep -q "PUT ok\|succeeded" || {
    echo "ERROR: PUT did not complete successfully (exit ${put_rc})"
    echo "Output: ${put_out}"
    exit 1
}

# Layer 2: RDMA GET
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

# Layer 3: data-plane transfer with payload verification
#
# v2-data-client performs a real RDMA transfer via the v2 protocol:
# PUT writes "dp-put-payload" into the server staging buffer, then GET
# reads it back and verifies the first 8 bytes match "dp-put-p".
# This is the only CI test that confirms bytes actually move.
DATA_CLIENT="${BUILD_DIR}/test/integration/rdma-test-server/v2-data-client"
SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

echo "--- RDMA data-plane PUT (payload verification) ---"
dp_put_out=""
dp_put_rc=0
dp_put_out=$("${DATA_CLIENT}" "${SERVER_HOST}" "${SERVER_PORT}" \
    PUT /bucket/dp-test 4096 2>&1) || dp_put_rc=$?
echo "${dp_put_out}"
if [ "${dp_put_rc}" -ne 0 ]; then
    echo "ERROR: data-plane PUT failed (exit ${dp_put_rc})"
    exit 1
fi

echo "--- RDMA data-plane GET (payload verification) ---"
dp_get_out=""
dp_get_rc=0
dp_get_out=$("${DATA_CLIENT}" "${SERVER_HOST}" "${SERVER_PORT}" \
    GET /bucket/dp-test 4096 2>&1) || dp_get_rc=$?
echo "${dp_get_out}"
echo "${dp_get_out}" | grep -q "payload verified" || {
    echo "ERROR: data-plane GET payload verification failed (exit ${dp_get_rc})"
    exit 1
}

echo "--- ernic integration: PASS ---"
