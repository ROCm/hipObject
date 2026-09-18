#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Test the rocm-ernic --backend s3 mode in a single container.
#
# rocm-ernic serves an in-process S3-over-RDMA object store at
# ${S3_IP}:${S3_PORT} via a TAP interface.  The minio-getput-rdma binary
# (pre-built, mounted at /hipobject-build) runs as the S3 client in the
# same container.
#
# Requires --cap-add NET_ADMIN (to create the TAP interface) or --privileged.
#
# Environment variables:
#   TEST_SIZE   - transfer size in bytes  (default: 65536)
#   S3_IP       - S3 endpoint IP          (default: 192.168.200.1)
#   S3_PORT     - S3 endpoint port        (default: 9000)
#   TAP_IFNAME  - TAP interface name      (default: ernicTap)
#   S3_BUCKET   - bucket name             (default: ernic)

set -euo pipefail

export LD_LIBRARY_PATH=/hipobject-build/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

TEST_SIZE="${TEST_SIZE:-65536}"
S3_IP="${S3_IP:-192.168.200.1}"
S3_PORT="${S3_PORT:-9000}"
TAP_IFNAME="${TAP_IFNAME:-ernicTap}"
S3_BUCKET="${S3_BUCKET:-ernic}"
BUILD_DIR=/hipobject-build

echo "=== ernic S3 backend test ==="
echo "    size:     ${TEST_SIZE} bytes"
echo "    endpoint: http://${S3_IP}:${S3_PORT}"

# Create the TAP interface so rocm-ernic can bind its S3 endpoint to it.
ip tuntap add dev "${TAP_IFNAME}" mode tap 2>/dev/null || true
ip link set "${TAP_IFNAME}" up

# Start rocm-ernic with the S3 backend.  It assigns 192.168.200.1 to the TAP
# and listens for S3-over-RDMA connections on port ${S3_PORT}.
rocm-ernic \
    --backend "s3:bucket=${S3_BUCKET},ip=${S3_IP},port=${S3_PORT}" \
    --tap "${TAP_IFNAME}" \
    --log-level info &
ERNIC_PID=$!

# Wait for the S3 endpoint to accept connections.
echo "Waiting for S3 endpoint at ${S3_IP}:${S3_PORT}..."
for i in $(seq 1 30); do
    if curl -sf "http://${S3_IP}:${S3_PORT}/${S3_BUCKET}?location" \
            -o /dev/null 2>/dev/null; then
        echo "S3 backend ready (attempt ${i})"
        break
    fi
    # Also accept a 403/404 — any HTTP response means the server is up.
    code=$(curl -s -o /dev/null -w "%{http_code}" \
           "http://${S3_IP}:${S3_PORT}/${S3_BUCKET}?location" 2>/dev/null || echo 0)
    if [ "${code}" -ge 200 ] 2>/dev/null; then
        echo "S3 backend ready (attempt ${i}, HTTP ${code})"
        break
    fi
    if [ "${i}" -eq 30 ]; then
        echo "ERROR: S3 backend did not become ready"
        kill "${ERNIC_PID}" 2>/dev/null || true
        exit 1
    fi
    sleep 1
done

echo "--- minio-cpp bridge PUT + GET via rocm-ernic S3 backend ---"
echo "    server:   http://${S3_IP}:${S3_PORT}"
echo "    size:     ${TEST_SIZE} bytes"
echo "    gpu mode: nogpu"

T_START=$(date +%s%N)

out=""
rc=0
out=$("${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" \
    "${S3_IP}:${S3_PORT}" \
    minioadmin minioadmin \
    "${TEST_SIZE}" nogpu 2>&1) || rc=$?
echo "${out}"

T_END=$(date +%s%N)
ELAPSED_MS=$(( (T_END - T_START) / 1000000 ))
TOTAL_BYTES=$(( 2 * TEST_SIZE ))
if [ "${ELAPSED_MS}" -gt 0 ]; then
    THROUGHPUT_MBPS=$(( TOTAL_BYTES * 1000 / ELAPSED_MS / 1024 / 1024 ))
    echo "Throughput: ${THROUGHPUT_MBPS} MB/s (PUT+GET ${TOTAL_BYTES} bytes in ${ELAPSED_MS} ms)"
fi

kill "${ERNIC_PID}" 2>/dev/null || true

echo "${out}" | grep -q "Data integrity check passed" || {
    echo "ERROR: S3 backend payload verification failed (exit ${rc})"
    exit 1
}

echo "--- ernic S3 backend integration: PASS ---"
