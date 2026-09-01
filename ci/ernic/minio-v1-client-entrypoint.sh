#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run pre-built minio-getput-rdma inside the ernic container against
# hipobj-rdma-test-server in v1 mode.  Binary built on runner, mounted
# read-only at /hipobject-build.
#
# Environment variables:
#   SERVER_ENDPOINT  - http URL of the test server (default: http://ernic-server:9000)
#   TEST_SIZE        - transfer size in bytes       (default: 65536)

set -euo pipefail

export LD_LIBRARY_PATH=/hipobject-build/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://ernic-server:9000}"
TEST_SIZE="${TEST_SIZE:-65536}"
BUILD_DIR=/hipobject-build

SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

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
