#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run pre-built minio-getput-rdma inside the ernic+rocjitsu client container
# against hipobj-rdma-test-server in v1 mode.  Binary built on runner, mounted
# read-only at /hipobject-build.
#
# Environment variables:
#   SERVER_ENDPOINT    - http URL of the test server (default: http://ernic-server:9000)
#   TEST_SIZE          - transfer size in bytes       (default: 65536)
#   ROCJITSU_SOCKET    - vfio-user socket for rocjitsu (default: /tmp/vfio-sockets/rocjitsu.sock)
#   ROCJITSU_CONFIG    - rocjitsu GPU config JSON     (default: gfx950_mi355x.json)

set -euo pipefail

export LD_LIBRARY_PATH=/hipobject-build/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# Start rocm-ernic (emulated ionic NIC, loopback backend)
rocm-ernic --backend loopback &
ERNIC_PID=$!
sleep 1

export HIPOBJ_NIC_HINT="${HIPOBJ_NIC_HINT:-ernic0}"
echo "Using NIC hint: ${HIPOBJ_NIC_HINT}"

# rocjitsu provides a vfio-user GPU socket, but the ernic container does not have
# the /dev/kfd + /dev/dri devices that HIP requires for hipMalloc.  GPU mode
# will be enabled once rocjitsu is integrated via a full VM setup.
ROCJITSU_SOCKET="${ROCJITSU_SOCKET:-/tmp/vfio-sockets/rocjitsu.sock}"
ROCJITSU_CONFIG="${ROCJITSU_CONFIG:-gfx950_mi355x.json}"
GPU_MODE="nogpu"
echo "Running in nogpu mode (GPU mode requires /dev/kfd + /dev/dri)"

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://ernic-server:9000}"
TEST_SIZE="${TEST_SIZE:-65536}"
BUILD_DIR=/hipobject-build

SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

echo "--- minio-cpp bridge v1 PUT + GET ---"
echo "    server:   ${SERVER_ENDPOINT}"
echo "    size:     ${TEST_SIZE} bytes"
echo "    gpu mode: ${GPU_MODE}"

# Throughput measurement: time the full PUT+GET round-trip
T_START=$(date +%s%N)

out=""
rc=0
out=$("${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" \
    "${SERVER_HOST}:${SERVER_PORT}" \
    minioadmin minioadmin \
    "${TEST_SIZE}" "${GPU_MODE}" 2>&1) || rc=$?
echo "${out}"

T_END=$(date +%s%N)
ELAPSED_NS=$(( T_END - T_START ))
ELAPSED_MS=$(( ELAPSED_NS / 1000000 ))

# Two transfers (PUT + GET): total bytes transferred = 2 * TEST_SIZE
TOTAL_BYTES=$(( 2 * TEST_SIZE ))
# Throughput in MB/s (integer arithmetic; *1000 to avoid float)
if [ "${ELAPSED_MS}" -gt 0 ]; then
    THROUGHPUT_KBPS=$(( TOTAL_BYTES * 1000 / ELAPSED_MS / 1024 ))
    THROUGHPUT_MBPS=$(( THROUGHPUT_KBPS / 1024 ))
    echo "Throughput: ${THROUGHPUT_MBPS} MB/s (PUT+GET ${TOTAL_BYTES} bytes in ${ELAPSED_MS} ms)"
fi

# Data correctness
echo "${out}" | grep -q "Data integrity check passed" || {
    echo "ERROR: minio-cpp bridge v1 payload verification failed (exit ${rc})"
    kill "${ERNIC_PID}" 2>/dev/null || true
    exit 1
}

echo "--- ernic minio-v1 integration: PASS ---"
kill "${ERNIC_PID}" 2>/dev/null || true
