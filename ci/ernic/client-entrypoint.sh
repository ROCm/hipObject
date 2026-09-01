#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run pre-built hipObject example and v2-data-client binaries inside the
# ernic container against hipobj-rdma-test-server.  All binaries are built
# on the runner (ROCm container) and mounted read-only at /hipobject-build.
#
# Environment variables (all have defaults):
#   SERVER_ENDPOINT  - http URL of the test server (default: http://ernic-server:9000)
#   TEST_SIZE        - object size in bytes         (default: 1048576)

set -euo pipefail

export LD_LIBRARY_PATH=/hipobject-build/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

rocm-ernic --backend loopback &
sleep 1

# Pass the rocm-ernic device name as a NIC hint so hipObjInit can find
# the device without GPU topology. rocm-ernic registers as ernic0.
export HIPOBJ_NIC_HINT="${HIPOBJ_NIC_HINT:-ernic0}"
echo "Using NIC hint: ${HIPOBJ_NIC_HINT}"

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://ernic-server:9000}"
TEST_SIZE="${TEST_SIZE:-1048576}"
BUILD_DIR=/hipobject-build
BUCKET=hipobj-ci
OBJECT=ernic-test-object

SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

# Layer 2: control-plane PUT + GET via curl ops
echo "--- RDMA PUT ---"
put_out=""
put_rc=0
put_out=$("${BUILD_DIR}/examples/put-object" \
    "${TEST_SIZE}" \
    --live "${SERVER_ENDPOINT}" \
    "${BUCKET}" "${OBJECT}" 2>&1) || put_rc=$?
echo "${put_out}"
echo "${put_out}" | grep -q "PUT ok\|succeeded" || {
    echo "ERROR: PUT did not complete successfully (exit ${put_rc})"
    echo "Output: ${put_out}"
    exit 1
}

echo "--- RDMA GET ---"
get_out=""
get_rc=0
get_out=$("${BUILD_DIR}/examples/get-object" \
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
echo "--- RDMA data-plane PUT (payload verification) ---"
dp_put_out=""
dp_put_rc=0
dp_put_out=$("${BUILD_DIR}/test/integration/rdma-test-server/v2-data-client" \
    "${SERVER_HOST}" "${SERVER_PORT}" \
    PUT /bucket/dp-test 4096 2>&1) || dp_put_rc=$?
echo "${dp_put_out}"
if [ "${dp_put_rc}" -ne 0 ]; then
    echo "ERROR: data-plane PUT failed (exit ${dp_put_rc})"
    exit 1
fi

echo "--- RDMA data-plane GET (payload verification) ---"
dp_get_out=""
dp_get_rc=0
dp_get_out=$("${BUILD_DIR}/test/integration/rdma-test-server/v2-data-client" \
    "${SERVER_HOST}" "${SERVER_PORT}" \
    GET /bucket/dp-test 4096 2>&1) || dp_get_rc=$?
echo "${dp_get_out}"
echo "${dp_get_out}" | grep -q "payload verified" || {
    echo "ERROR: data-plane GET payload verification failed (exit ${dp_get_rc})"
    exit 1
}

echo "--- ernic integration: PASS ---"
