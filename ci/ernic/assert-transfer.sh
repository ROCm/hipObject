#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Assert what a minio-getput-rdma run actually did, from its captured output.
#
# Usage: assert-transfer.sh <log-file> <rdma|http>
#
# Two things are checked, and both exist because a run that "passed" tells you
# almost nothing on its own:
#
#   1. Data verification. The example writes a seeded pattern, clobbers the
#      buffer, GETs the same key back and compares. Without the
#      "Data integrity check passed" line a zero exit only means nothing
#      crashed.
#
#   2. Transport. hipobj::minio::Client falls back to plain HTTP when
#      hipObjInit finds no NIC -- silently -- and again when the RDMA
#      transfer itself fails, and returns success either way. So a lane that
#      lost its emulated wire entirely still goes green. The example prints a
#      hipobj-stats: line from the bridge's counters; this checks the
#      transport the lane is supposed to be exercising is the one that moved
#      the bytes.
#
# Mode "rdma": every PUT and GET went over the wire, none fell back.
# Mode "http": the inverse, for lanes with no verbs device by construction.

set -uo pipefail

LOG="${1:?usage: assert-transfer.sh <log-file> <rdma|http>}"
MODE="${2:?usage: assert-transfer.sh <log-file> <rdma|http>}"

fail() {
    echo "ERROR: $*"
    echo "--- captured output ---"
    cat "${LOG}" || true
    exit 1
}

grep -q "Data integrity check passed" "${LOG}" \
    || fail "the GET did not return what the PUT wrote (no integrity line)"

stats=$(grep -m1 '^hipobj-stats:' "${LOG}") \
    || fail "no hipobj-stats: line -- the client did not report its transport"

get() { echo "${stats}" | tr ' ' '\n' | sed -n "s/^$1=//p"; }

rdma_put=$(get rdma_put); rdma_get=$(get rdma_get)
http_put=$(get http_put); http_get=$(get http_get)

for v in "${rdma_put}" "${rdma_get}" "${http_put}" "${http_get}"; do
    [ -n "${v}" ] || fail "malformed stats line: ${stats}"
done

echo "${stats}"

case "${MODE}" in
    rdma)
        [ "${rdma_put}" -ge 1 ] || fail "no PUT went over RDMA: ${stats}"
        [ "${rdma_get}" -ge 1 ] || fail "no GET came back over RDMA: ${stats}"
        if [ "${http_put}" -ne 0 ] || [ "${http_get}" -ne 0 ]; then
            fail "fell back to TCP/IP: ${stats}"
        fi
        echo "--- verified: payload matched, all transfers over RDMA ---"
        ;;
    http)
        [ "${http_put}" -ge 1 ] || fail "no PUT over HTTP: ${stats}"
        [ "${http_get}" -ge 1 ] || fail "no GET over HTTP: ${stats}"
        echo "--- verified: payload matched, HTTP path exercised ---"
        ;;
    *)
        fail "unknown mode '${MODE}' (expected rdma or http)"
        ;;
esac
