#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Compare the digest pinned in .cursor/Dockerfile to the digest Docker Hub
# currently serves for the image in the # track: comment (e.g. 7.2 on
# rocm/dev-ubuntu-24.04). Exit 0 when they match; exit 1 when the tag moved.
#
# Usage: from repository root (any cwd works):
#   bash .cursor/scripts/check-cursor-rocm-image-pin.sh

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
readonly DOCKERFILE="${REPO_ROOT}/.cursor/Dockerfile"

if [[ ! -f "${DOCKERFILE}" ]]; then
    echo "check-cursor-rocm-image-pin: missing ${DOCKERFILE}" >&2
    exit 2
fi

from_line="$(grep -E '^[[:space:]]*FROM[[:space:]]+' "${DOCKERFILE}" | head -1 || true)"
if [[ -z "${from_line}" ]]; then
    echo "check-cursor-rocm-image-pin: no FROM line in ${DOCKERFILE}" >&2
    exit 2
fi

if [[ "${from_line}" != *"@sha256:"* ]]; then
    echo "check-cursor-rocm-image-pin: FROM must use @sha256: digest pinning" >&2
    exit 2
fi

pinned_digest="$(printf '%s\n' "${from_line}" | sed -n 's/.*@\(sha256:[a-f0-9]\{64\}\).*/\1/p')"
if [[ -z "${pinned_digest}" ]]; then
    echo "check-cursor-rocm-image-pin: could not parse pinned digest" >&2
    exit 2
fi

track_line="$(grep -E '^[[:space:]]*#[[:space:]]*track:[[:space:]]+' "${DOCKERFILE}" | head -1 || true)"
track_ref="${track_line#*track:}"
track_ref="$(printf '%s\n' "${track_ref}" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
if [[ -z "${track_ref}" ]]; then
    echo "check-cursor-rocm-image-pin: add '# track: namespace/name:tag' above FROM" >&2
    exit 2
fi

if [[ "${track_ref}" != *:* ]]; then
    echo "check-cursor-rocm-image-pin: track ref must include a tag (namespace/name:tag)" >&2
    exit 2
fi

image_path="${track_ref%:*}"
tag="${track_ref##*:}"
if [[ -z "${image_path}" || -z "${tag}" ]]; then
    echo "check-cursor-rocm-image-pin: could not split track ref '${track_ref}'" >&2
    exit 2
fi

remote_digest=""

if command -v docker >/dev/null 2>&1; then
    if remote_digest="$(docker buildx imagetools inspect "${track_ref}" --format '{{.Manifest.Digest}}' 2>/dev/null)"; then
        :
    else
        remote_digest=""
    fi
fi

if [[ -z "${remote_digest}" ]] && command -v skopeo >/dev/null 2>&1; then
    remote_digest="$(skopeo inspect --format '{{.Digest}}' "docker://${track_ref}" 2>/dev/null || true)"
fi

if [[ -z "${remote_digest}" ]]; then
    hub_url="https://hub.docker.com/v2/repositories/${image_path}/tags/${tag}/"
    if ! hub_json="$(curl -fsS "${hub_url}" 2>/dev/null)"; then
        echo "check-cursor-rocm-image-pin: failed to query ${hub_url}" >&2
        echo "Install Docker (buildx), skopeo, or fix TLS/network to Docker Hub." >&2
        exit 2
    fi
    remote_digest="$(printf '%s\n' "${hub_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("digest") or (d.get("images") or [{}])[0].get("digest",""))')" || true
fi

if [[ -z "${remote_digest}" ]]; then
    echo "check-cursor-rocm-image-pin: could not resolve remote digest for ${track_ref}" >&2
    exit 2
fi

remote_digest="$(printf '%s' "${remote_digest}" | tr -d '\n\r[:space:]')"
pinned_digest="$(printf '%s' "${pinned_digest}" | tr -d '\n\r[:space:]')"

normalize() {
    local d="$1"
    d="${d#sha256:}"
    printf '%s' "${d}"
}

pinned_n="$(normalize "${pinned_digest}")"
remote_n="$(normalize "${remote_digest}")"

if [[ "${pinned_n}" == "${remote_n}" ]]; then
    echo "OK: pinned digest matches ${track_ref} (${pinned_digest})"
    exit 0
fi

echo "UPDATE AVAILABLE: ${track_ref} registry digest differs from Dockerfile pin." >&2
echo "  pinned:  ${pinned_digest}" >&2
echo "  remote:  ${remote_digest}" >&2
echo "Refresh FROM in .cursor/Dockerfile and re-pin apt versions (see AGENTS.md)." >&2
exit 1
