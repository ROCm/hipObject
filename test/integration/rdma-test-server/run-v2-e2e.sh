#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# End-to-end run of the v2 reference server and the control harness.
# The wrapper supervises the server with a hard timeout; exit codes
# 124/137 mean the supervisor reclaimed a hung process (pass when a
# hang was requested via --hang-check).

set -u

PORT="${PORT:-9443}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SERVER="${SERVER:-$HERE/../../build/hipobj-rdma-test-server}"
HARNESS="${HARNESS:-$HERE/../../build/v2-client-harness}"

if [ ! -x "$SERVER" ]; then
    echo "server binary missing: $SERVER" >&2
    exit 2
fi
if [ ! -x "$HARNESS" ]; then
    echo "harness binary missing: $HARNESS" >&2
    exit 2
fi

mode="${1:-run}"
if [ "$mode" = "--hang-check" ]; then
    timeout --kill-after=10 120 "$SERVER" "$PORT" --v2 --hang-after-prepare
    rc=$?
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        echo "hang reclaimed by supervisor (rc=$rc)"
        exit 0
    fi
    echo "expected supervisor reclaim, got rc=$rc" >&2
    exit 1
fi

timeout --kill-after=10 120 "$SERVER" "$PORT" --v2 &
server_pid=$!
trap 'kill $server_pid 2>/dev/null; wait $server_pid 2>/dev/null' EXIT

# Wait for the listener.
for _ in $(seq 1 50); do
    if grep -q "" < /dev/tcp/127.0.0.1/"$PORT" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

fail=0

run_case() {
    local desc="$1"; shift
    if "$@"; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc"
        fail=1
    fi
}

# Success path: GET.
run_case "prepare+ready GET 200 + cookie echo" \
    "$HARNESS" 127.0.0.1 "$PORT" GET /bucket/seed64k 65536 --expect 200

# Preserve-error: mismatched READY cookie answers 403.
run_case "ready mismatched cookie 403" \
    "$HARNESS" 127.0.0.1 "$PORT" GET /bucket/seed64k 65536 \
    --ready-cookie-hex deadbeef --expect 403

# Over-cap size rejected at PREPARE.
run_case "prepare 2GiB cap 413" \
    "$HARNESS" 127.0.0.1 "$PORT" GET /bucket/big 2147483648 --expect 413

# Cancel idempotency.
run_case "cancel idempotent 204/204" \
    "$HARNESS" 127.0.0.1 "$PORT" GET /bucket/seed64k 65536 \
    --expect 200 --cancel

kill "$server_pid" 2>/dev/null
wait "$server_pid" 2>/dev/null
trap - EXIT
exit $fail
