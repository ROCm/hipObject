#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run ernic integration tests locally using pre-built or freshly-built binaries.
#
# Usage:
#   ci/ernic/run-local.sh                       # v2 test, use build-v2 binaries
#   ci/ernic/run-local.sh --rebuild             # v2 test, force ROCm container build
#   ci/ernic/run-local.sh --minio-v1            # v1 minio-cpp test, use build-minio binaries
#   ci/ernic/run-local.sh --minio-v1 --rebuild  # v1 minio-cpp test, force build
#   ci/ernic/run-local.sh --s3-backend          # rocm-ernic --backend s3 test (requires --privileged)

set -euo pipefail

REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
ERNIC_IMAGE=${ERNIC_IMAGE:-sbates130272/batesste-ci-images-ubuntu-rocm-ernic:latest}
ROCJITSU_IMAGE=${ROCJITSU_IMAGE:-sbates130272/batesste-ci-images-ubuntu-rocm-rocjitsu:august-28-2026}
ROCM_IMAGE=rocm/dev-ubuntu-24.04:7.14.0-full
AMD_CA_CERT=/home/stebates/Projects/batesste-ci-images/common/amd-root-ca.crt

MINIO_V1=false
S3_BACKEND=false
REBUILD=false

for arg in "$@"; do
    case "${arg}" in
        --minio-v1)    MINIO_V1=true    ;;
        --s3-backend)  S3_BACKEND=true  ;;
        --rebuild)     REBUILD=true     ;;
        *) echo "Unknown argument: ${arg}"; exit 1 ;;
    esac
done

# ── v2 protocol test ──────────────────────────────────────────────────────────

run_v2() {
    local BUILD_DIR="${REPO}/build-v2"

    if [ "${REBUILD}" = true ]; then
        BUILD_DIR=/tmp/ernic-rebuild
        mkdir -p "${BUILD_DIR}/rocm-libs"
        echo "=== Rebuilding (v2) in ROCm container ==="
        _rocm_build "${BUILD_DIR}" "OFF" \
            "hipobj-rdma-test-server put-object get-object v2-data-client"
    else
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
        echo "=== Server logs (v2) ==="
        docker logs ernic-server-local 2>&1 || true
        docker rm -f ernic-server-local 2>/dev/null || true
        docker network rm ernic-local-net 2>/dev/null || true
    }
    trap cleanup EXIT

    echo "=== Starting ernic server (v2) ==="
    docker run -d \
        --name ernic-server-local \
        --hostname ernic-server \
        --network ernic-local-net \
        -v "${BUILD_DIR}:/hipobject-build:ro" \
        -v "${REPO}:/hipobject:ro" \
        --entrypoint /hipobject/ci/ernic/server-entrypoint.sh \
        "$ERNIC_IMAGE"

    _wait_for_server ernic-server-local 9000

    echo "=== Running ernic client (v2) ==="
    docker run --rm \
        --name ernic-client-local \
        --network ernic-local-net \
        -v "${BUILD_DIR}:/hipobject-build:ro" \
        -v "${REPO}:/hipobject:ro" \
        -e SERVER_ENDPOINT=http://ernic-server:9000 \
        -e TEST_SIZE=65536 \
        --entrypoint /hipobject/ci/ernic/client-entrypoint.sh \
        "$ERNIC_IMAGE"
}

# ── minio-v1 protocol test ────────────────────────────────────────────────────

run_minio_v1() {
    local BUILD_DIR="${REPO}/build-minio"

    if [ "${REBUILD}" = true ]; then
        BUILD_DIR=/tmp/ernic-minio-rebuild
        mkdir -p "${BUILD_DIR}/rocm-libs"
        echo "=== Rebuilding (minio-v1) in ROCm container ==="
        _rocm_build "${BUILD_DIR}" "ON" \
            "hipobj-rdma-test-server minio-getput-rdma"
    else
        if [ ! -x "${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" ]; then
            echo "ERROR: ${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma not found"
            echo "Run 'cmake --build build-minio --target minio-getput-rdma hipobj-rdma-test-server' first"
            exit 1
        fi
        echo "=== Using existing build-minio binaries ==="
    fi

    chmod +x "${REPO}/ci/ernic/server-v1-entrypoint.sh" \
             "${REPO}/ci/ernic/minio-v1-client-entrypoint.sh"

    VFIO_SOCK_DIR="$(mktemp -d /tmp/vfio-sockets-XXXXX)"

    docker network create ernic-minio-local-net 2>/dev/null || true

    cleanup() {
        echo "=== Server logs (v1) ==="
        docker logs ernic-server-v1-local 2>&1 || true
        echo "=== rocjitsu logs ==="
        docker logs rocjitsu-local 2>&1 || true
        docker rm -f ernic-server-v1-local rocjitsu-local 2>/dev/null || true
        docker network rm ernic-minio-local-net 2>/dev/null || true
        rm -rf "${VFIO_SOCK_DIR}"
    }
    trap cleanup EXIT

    echo "=== Starting rocjitsu (GPU emulator) ==="
    docker run -d \
        --name rocjitsu-local \
        --network ernic-minio-local-net \
        -v "${VFIO_SOCK_DIR}:/tmp/vfio-sockets" \
        "$ROCJITSU_IMAGE" \
        rocjitsu \
            --config /usr/local/share/rocjitsu/configs/gfx950_mi355x.json \
            --vfio-socket /tmp/vfio-sockets/rocjitsu.sock

    echo "=== Starting ernic server (v1) ==="
    docker run -d \
        --name ernic-server-v1-local \
        --hostname ernic-server \
        --network ernic-minio-local-net \
        -v "${BUILD_DIR}:/hipobject-build:ro" \
        -v "${REPO}:/hipobject:ro" \
        --entrypoint /hipobject/ci/ernic/server-v1-entrypoint.sh \
        "$ERNIC_IMAGE"

    _wait_for_server ernic-server-v1-local 9000

    echo "=== Running ernic minio-v1 client ==="
    docker run --rm \
        --name ernic-minio-client-local \
        --network ernic-minio-local-net \
        -v "${BUILD_DIR}:/hipobject-build:ro" \
        -v "${REPO}:/hipobject:ro" \
        -v "${VFIO_SOCK_DIR}:/tmp/vfio-sockets" \
        -e SERVER_ENDPOINT=http://ernic-server:9000 \
        -e TEST_SIZE=65536 \
        -e ROCJITSU_SOCKET=/tmp/vfio-sockets/rocjitsu.sock \
        --entrypoint /hipobject/ci/ernic/minio-v1-client-entrypoint.sh \
        "$ERNIC_IMAGE"
}

# ── helpers ───────────────────────────────────────────────────────────────────

_rocm_build() {
    local BUILD_DIR="$1"
    local MINIO_ON="$2"
    local TARGETS="$3"

    local ca_args=""
    if [ -f "${AMD_CA_CERT}" ]; then
        ca_args="-v ${AMD_CA_CERT}:/tmp/amd-root-ca.crt:ro"
    fi

    # shellcheck disable=SC2086
    docker run --rm \
        --user root \
        -v "${REPO}:/hipobject:ro" \
        -v "${BUILD_DIR}:/hipobject-build" \
        ${ca_args} \
        -e ROCM_PATH=/opt/rocm \
        "$ROCM_IMAGE" \
        bash -c "
          if [ -f /tmp/amd-root-ca.crt ]; then
              cp /tmp/amd-root-ca.crt /usr/local/share/ca-certificates/amd-root-ca.crt && \
              update-ca-certificates
          fi
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
            -DHIPOBJ_MINIO_CLIENT=${MINIO_ON} \
            -DHIPOBJ_BUILD_DOCS=OFF \
            $([ "${MINIO_ON}" = "ON" ] && echo "-DCMAKE_SKIP_INSTALL_RULES=ON") && \
          cmake --build /hipobject-build --parallel \$(nproc) \
            --target ${TARGETS} && \
          mkdir -p /hipobject-build/rocm-libs && \
          find /opt/rocm -name '*.so*' \( -type f -o -type l \) \
            -exec cp -P {} /hipobject-build/rocm-libs/ \; 2>/dev/null || true
        "
}

_wait_for_server() {
    local CONTAINER="$1"
    local PORT="$2"
    echo "Waiting for server on port ${PORT}..."
    for i in $(seq 1 30); do
        if docker exec "${CONTAINER}" \
                bash -c "(echo > /dev/tcp/127.0.0.1/${PORT}) 2>/dev/null"; then
            echo "Server ready (attempt $i)"; return 0
        fi
        if [ "$i" -eq 30 ]; then
            echo "ERROR: server did not become ready"
            return 1
        fi
        sleep 2
    done
}

# ── s3-backend test ───────────────────────────────────────────────────────────

run_s3_backend() {
    local BUILD_DIR="${REPO}/build-minio"

    if [ "${REBUILD}" = true ]; then
        BUILD_DIR=/tmp/ernic-s3-rebuild
        mkdir -p "${BUILD_DIR}/rocm-libs"
        echo "=== Rebuilding (s3-backend) in ROCm container ==="
        _rocm_build "${BUILD_DIR}" "ON" "minio-getput-rdma"
    else
        if [ ! -x "${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma" ]; then
            echo "ERROR: ${BUILD_DIR}/integrations/minio-cpp/minio-getput-rdma not found"
            echo "Run with --rebuild or build build-minio first"
            exit 1
        fi
        echo "=== Using existing build-minio binaries ==="
    fi

    chmod +x "${REPO}/ci/ernic/s3-backend-entrypoint.sh"

    echo "=== Running S3 backend test (single privileged container) ==="
    docker run --rm \
        --name ernic-s3-backend-local \
        --cap-add NET_ADMIN \
        -v "${BUILD_DIR}:/hipobject-build:ro" \
        -v "${REPO}:/hipobject:ro" \
        -e TEST_SIZE=65536 \
        --entrypoint /hipobject/ci/ernic/s3-backend-entrypoint.sh \
        "$ERNIC_IMAGE"
}

# ── dispatch ──────────────────────────────────────────────────────────────────

if [ "${S3_BACKEND}" = true ]; then
    run_s3_backend
elif [ "${MINIO_V1}" = true ]; then
    run_minio_v1
else
    run_v2
fi
