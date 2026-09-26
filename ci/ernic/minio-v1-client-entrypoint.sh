#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Drive the minio-cpp bridge (minio-getput-rdma) against hipobj-rdma-test-server
# in v1 mode, time the round trip, and assert what the transfer actually did.
#
# Two callers, and they differ in one thing only -- where the verbs device
# comes from:
#
#   The MinIO Bridge CI lane runs this inside a guest VM whose ionic driver
#   has bound an emulated PCI function served by a rocm-ernic instance on the
#   runner, meshed to the server VM's. There is a real wire, so the defaults
#   apply: do not start an emulator in here, and require RDMA.
#
#   docker-compose's minio-v1 profile runs it in a container, where
#   START_ERNIC=true brings up a loopback rocm-ernic for the vfio-user socket
#   but nothing binds it -- a container has no guest kernel, so
#   /sys/class/infiniband stays empty and the bridge falls back to HTTP.
#   That profile passes EXPECT_TRANSPORT=http and gets exactly that.
#
# Environment variables:
#   BUILD_DIR          - where the binaries live     (default: /hipobject-build)
#   SERVER_ENDPOINT    - http URL of the test server (default: http://ernic-server:9000)
#   TEST_SIZE          - transfer size in bytes      (default: 65536)
#   GPU_MODE           - gpu or nogpu                (default: nogpu)
#   START_ERNIC        - start a loopback rocm-ernic in here (default: false)
#   EXPECT_TRANSPORT   - rdma or http, asserted against the bridge's own
#                        transfer counters           (default: rdma)
#   ROCJITSU_SOCKET    - vfio-user socket for rocjitsu, informational only

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/hipobject-build}"
export LD_LIBRARY_PATH="${BUILD_DIR}/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ERNIC_PID=
cleanup() {
    if [ -n "${ERNIC_PID}" ]; then
        kill "${ERNIC_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if [ "${START_ERNIC:-false}" = true ]; then
    rocm-ernic --backend loopback &
    ERNIC_PID=$!
    sleep 1
fi

export HIPOBJ_NIC_HINT="${HIPOBJ_NIC_HINT:-ernic0}"
echo "Using NIC hint: ${HIPOBJ_NIC_HINT}"

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://ernic-server:9000}"
TEST_SIZE="${TEST_SIZE:-65536}"
GPU_MODE="${GPU_MODE:-nogpu}"

SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

echo "--- minio-cpp bridge v1 PUT + GET ---"
echo "    server:   ${SERVER_ENDPOINT}"
echo "    size:     ${TEST_SIZE} bytes"
echo "    gpu mode: ${GPU_MODE}"
echo "    expect:   ${EXPECT_TRANSPORT:-rdma}"

T_START=$(date +%s%N)

out=""
rc=0
out=$("${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" \
    "${SERVER_HOST}:${SERVER_PORT}" \
    minioadmin minioadmin \
    "${TEST_SIZE}" "${GPU_MODE}" 2>&1) || rc=$?
echo "${out}"

T_END=$(date +%s%N)
ELAPSED_MS=$(( (T_END - T_START) / 1000000 ))

# Two transfers (PUT + GET): total bytes transferred = 2 * TEST_SIZE
TOTAL_BYTES=$(( 2 * TEST_SIZE ))
if [ "${ELAPSED_MS}" -gt 0 ]; then
    THROUGHPUT_MBPS=$(( TOTAL_BYTES * 1000 / ELAPSED_MS / 1024 / 1024 ))
    echo "Throughput: ${THROUGHPUT_MBPS} MB/s (PUT+GET ${TOTAL_BYTES} bytes in ${ELAPSED_MS} ms)"
fi

# Exit code alone is not evidence: the bridge returns success when it falls
# back to plain HTTP, so the assertion is on what the run reported doing.
LOG=$(mktemp)
printf '%s\n' "${out}" > "${LOG}"
"$(dirname "$0")/assert-transfer.sh" "${LOG}" "${EXPECT_TRANSPORT:-rdma}" || {
    echo "ERROR: minio-cpp bridge v1 verification failed (exit ${rc})"
    exit 1
}
rm -f "${LOG}"

echo "--- minio-cpp bridge v1 integration (${GPU_MODE}): PASS (exit ${rc}) ---"
