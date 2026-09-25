#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the pre-built minio-getput-rdma inside an ernic guest VM against
# hipobj-rdma-test-server (v1 mode) running in a second guest. All binaries
# are built on the runner (ROCm container) and copied into the guest at
# ${BUILD_DIR}.
#
# nogpu, not gpu: this lane emulates a NIC and nothing else, so hipMalloc
# returns hipErrorNoDevice before any of the RDMA path is reached. nogpu mode
# drives the same v1 transfer from a page-aligned host buffer and verifies
# the payload, which is the part that depends on the emulated wire. The
# GPU-buffer form is hipobject-hardware-test-gpu-direct.yml's job.
#
# Environment variables (all have defaults):
#   BUILD_DIR        - where the binaries were copied (default: /tmp/hipobject-build)
#   SERVER_ENDPOINT  - http URL of the test server (default: http://192.168.200.10:9000)
#   TEST_SIZE        - object size in bytes         (default: 1048576)
#   HIPOBJ_NIC_HINT  - verbs device to bind to. The caller should pass the
#                      device find-rdma-device.sh discovered: udev renames
#                      ionic_%d twice, so the final name is not predictable.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/tmp/hipobject-build}"
export LD_LIBRARY_PATH="${BUILD_DIR}/rocm-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

export HIPOBJ_NIC_HINT="${HIPOBJ_NIC_HINT:-ernic0}"
echo "Using NIC hint: ${HIPOBJ_NIC_HINT}"

SERVER_ENDPOINT="${SERVER_ENDPOINT:-http://192.168.200.10:9000}"
TEST_SIZE="${TEST_SIZE:-1048576}"

SERVER_HOST="${SERVER_ENDPOINT#http://}"
SERVER_HOST="${SERVER_HOST%%:*}"
SERVER_PORT="${SERVER_ENDPOINT##*:}"

echo "--- v1 PUT + GET over the emulated wire ---"
echo "    server: ${SERVER_ENDPOINT}"
echo "    size:   ${TEST_SIZE} bytes"

out=""
rc=0
out=$("${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" \
    "${SERVER_HOST}:${SERVER_PORT}" \
    minioadmin minioadmin \
    "${TEST_SIZE}" nogpu 2>&1) || rc=$?
echo "${out}"

LOG=$(mktemp)
printf '%s\n' "${out}" > "${LOG}"

# Exit code alone is not evidence: the bridge returns success when it falls
# back to plain HTTP, so the assertion is on what the run reported doing.
"$(dirname "$0")/assert-transfer.sh" "${LOG}" rdma || exit 1
rm -f "${LOG}"

echo "--- ernic two-VM v1 integration: PASS (exit ${rc}) ---"
