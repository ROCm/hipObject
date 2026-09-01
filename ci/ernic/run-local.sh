#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the ernic two-container integration test locally using the existing
# build-v2 binaries. Avoids network issues (ZScaler/FetchContent) by
# skipping the ROCm container build entirely.
#
# Usage:
#   ci/ernic/run-local.sh              # use build-v2 (default)
#   ci/ernic/run-local.sh --rebuild    # force a fresh ROCm container build

set -euo pipefail

REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
ERNIC_IMAGE=sbates130272/batesste-ci-images-ubuntu-rocm-ernic:august-27-2026
ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.14.0-full
AMD_CA_CERT=/home/stebates/Projects/batesste-ci-images/common/amd-root-ca.crt
BUILD_DIR="${REPO}/build-v2"
REBUILD=${1:-}

if [ "${REBUILD}" = "--rebuild" ]; then
  BUILD_DIR=/tmp/ernic-rebuild
  mkdir -p "${BUILD_DIR}/rocm-libs"
  echo "=== Rebuilding in ROCm container (may need network) ==="
  docker run --rm \
    --user root \
    -v "${REPO}:/hipobject:ro" \
    -v "${BUILD_DIR}:/hipobject-build" \
    -v "${AMD_CA_CERT}:/tmp/amd-root-ca.crt:ro" \
    -e ROCM_PATH=/opt/rocm \
    "$ROCM_IMAGE" \
    bash -c "
      cp /tmp/amd-root-ca.crt /usr/local/share/ca-certificates/amd-root-ca.crt && \
      update-ca-certificates && \
      export GIT_SSL_CAINFO=/etc/ssl/certs/ca-certificates.crt && \
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
      find /opt/rocm -name '*.so*' \( -type f -o -type l \) \
        -exec cp -P {} /hipobject-build/rocm-libs/ \; 2>/dev/null || true
    "
else
  # Use the existing build-v2 tree — no network needed
  if [ ! -x "${BUILD_DIR}/test/integration/rdma-test-server/hipobj-rdma-test-server" ]; then
    echo "ERROR: ${BUILD_DIR}/test/integration/rdma-test-server/hipobj-rdma-test-server not found"
    echo "Run 'cmake --build build-v2 --target hipobj-rdma-test-server put-object get-object v2-data-client' first"
    exit 1
  fi
  echo "=== Using existing build-v2 binaries ==="
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
