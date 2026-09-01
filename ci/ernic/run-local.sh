#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the ernic two-container integration test locally.
# Replicates exactly what GitHub Actions does so you can iterate without
# pushing to CI.
#
# Usage:
#   ci/ernic/run-local.sh          # full build + test
#   ci/ernic/run-local.sh --no-build  # skip build (reuse previous BUILD_DIR)

set -euo pipefail

REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
ERNIC_IMAGE=sbates130272/batesste-ci-images-ubuntu-rocm-ernic:august-27-2026
ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.14.0-full
BUILD_DIR=/tmp/ernic-local-build
SKIP_BUILD=${1:-}

mkdir -p "${BUILD_DIR}/rocm-libs"

if [ "${SKIP_BUILD}" != "--no-build" ]; then
  echo "=== Building in ROCm container ==="
  docker run --rm \
    --user root \
    -v "${REPO}:/hipobject:ro" \
    -v "${BUILD_DIR}:/hipobject-build" \
    -e ROCM_PATH=/opt/rocm \
    "$ROCM_IMAGE" \
    bash -c "
      apt-get update -qq && \
      apt-get install -y -qq git cmake ninja-build libibverbs-dev libnuma-dev \
        libcurl4-openssl-dev libssl-dev && \
      cmake -B /hipobject-build -G Ninja -S /hipobject \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/opt/rocm \
        -DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/clang \
        -DBUILD_TESTING=ON \
        -DHIPOBJ_IONIC=ON \
        -DHIPOBJ_BNXT=OFF \
        -DHIPOBJ_INTEGRATION_TESTS=ON \
        -DHIPOBJ_MINIO_CLIENT=OFF \
        -DHIPOBJ_BUILD_DOCS=OFF && \
      cmake --build /hipobject-build --parallel \$(nproc) \
        --target hipobj-rdma-test-server put-object get-object v2-data-client && \
      mkdir -p /hipobject-build/rocm-libs && \
      find /opt/rocm -name '*.so*' \( -type f -o -type l \) \
        -exec cp -P {} /hipobject-build/rocm-libs/ \; 2>/dev/null || true
    "
else
  echo "=== Skipping build (--no-build) ==="
fi

chmod +x "${REPO}/ci/ernic/server-entrypoint.sh" \
         "${REPO}/ci/ernic/client-entrypoint.sh"

docker network create ernic-local-net 2>/dev/null || true

cleanup() {
  echo "=== Server logs ==="
  docker logs ernic-server-local 2>&1 || true
  docker rm -f ernic-server-local 2>/dev/null || true
  docker network rm ernic-local-net 2>/dev/null || true
}
trap cleanup EXIT

echo "=== Starting ernic server ==="
docker run -d \
  --name ernic-server-local \
  --hostname ernic-server \
  --network ernic-local-net \
  -v "${BUILD_DIR}:/hipobject-build:ro" \
  -v "${REPO}:/hipobject:ro" \
  --entrypoint /hipobject/ci/ernic/server-entrypoint.sh \
  "$ERNIC_IMAGE"

echo "Waiting for server on port 9000..."
for i in $(seq 1 30); do
  if docker exec ernic-server-local \
      bash -c '(echo > /dev/tcp/127.0.0.1/9000) 2>/dev/null'; then
    echo "Server ready (attempt $i)"; break
  fi
  if [ "$i" -eq 30 ]; then
    echo "ERROR: server did not become ready"
    exit 1
  fi
  sleep 2
done

echo "=== Running ernic client ==="
docker run --rm \
  --name ernic-client-local \
  --network ernic-local-net \
  -v "${BUILD_DIR}:/hipobject-build:ro" \
  -v "${REPO}:/hipobject:ro" \
  -e SERVER_ENDPOINT=http://ernic-server:9000 \
  -e TEST_SIZE=65536 \
  --entrypoint /hipobject/ci/ernic/client-entrypoint.sh \
  "$ERNIC_IMAGE"
