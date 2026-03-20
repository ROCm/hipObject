#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# setup-cuobject-apt.sh
#
# Convenience script that configures the NVIDIA
# CUDA apt repository on Ubuntu and installs the
# cuObject client and server packages.
#
# Reference:
#   https://docs.nvidia.com/cuda/
#     cuda-installation-guide-linux/
#     index.html#ubuntu-installation
#
# Usage:
#   sudo ./scripts/setup-cuobject-apt.sh
#
# The script is idempotent: it skips steps that
# have already been completed.

set -euo pipefail

# ── Helpers ─────────────────────────────────

die() { echo "ERROR: $*" >&2; exit 1; }

info() { echo "==> $*"; }

need_root() {
  if [[ $EUID -ne 0 ]]; then
    die "This script must be run as root " \
        "(use sudo)."
  fi
}

# ── Pre-flight checks ──────────────────────

need_root

if ! command -v lsb_release &>/dev/null; then
  die "lsb_release not found. " \
      "Is this Ubuntu?"
fi

DISTRO_ID=$(lsb_release -si)
if [[ "$DISTRO_ID" != "Ubuntu" ]]; then
  die "This script targets Ubuntu. " \
      "Detected: $DISTRO_ID"
fi

CODENAME=$(lsb_release -sc)
VERSION=$(lsb_release -sr | tr -d '.')

ARCH=$(dpkg --print-architecture)
case "$ARCH" in
  amd64) NVARCH="x86_64"  ;;
  arm64) NVARCH="sbsa"    ;;
  *)     die "Unsupported arch: $ARCH" ;;
esac

DISTRO_TAG="ubuntu${VERSION}"

info "Detected Ubuntu ${VERSION} " \
     "(${CODENAME}) on ${ARCH}"

# ── 1. Install cuda-keyring ─────────────────

KEYRING_PKG="cuda-keyring"
KEYRING_VER="1.1-1"
KEYRING_DEB="${KEYRING_PKG}_${KEYRING_VER}_all.deb"
KEYRING_URL="https://developer.download.nvidia.com"
KEYRING_URL="${KEYRING_URL}/compute/cuda/repos"
KEYRING_URL="${KEYRING_URL}/${DISTRO_TAG}"
KEYRING_URL="${KEYRING_URL}/${NVARCH}/${KEYRING_DEB}"

if dpkg -s "$KEYRING_PKG" &>/dev/null; then
  info "cuda-keyring already installed, skipping."
else
  info "Downloading ${KEYRING_DEB}..."
  TMPDIR=$(mktemp -d)
  trap 'rm -rf "$TMPDIR"' EXIT
  wget -q -O "${TMPDIR}/${KEYRING_DEB}" \
    "$KEYRING_URL"
  info "Installing cuda-keyring..."
  dpkg -i "${TMPDIR}/${KEYRING_DEB}"
fi

# ── 2. Update package index ─────────────────

info "Updating apt package index..."
apt-get update -qq

# ── 3. Install cuObject client ──────────────

CLIENT_PKGS=(
  libcuobjclient
  libcuobjclient-dev
)

info "Installing cuObject client packages: " \
     "${CLIENT_PKGS[*]}"
apt-get install -y "${CLIENT_PKGS[@]}"

# ── 4. Install cuObject server ──────────────

SERVER_PKGS=(
  libcuobjserver
  libcuobjserver-dev
)

info "Installing cuObject server packages: " \
     "${SERVER_PKGS[*]}"
if ! apt-get install -y \
    "${SERVER_PKGS[@]}" 2>/dev/null; then
  echo ""
  echo "WARNING: cuObject server packages were"
  echo "not found in the repository.  The server"
  echo "library may require a separate download"
  echo "from NVIDIA.  See:"
  echo "  https://docs.nvidia.com/gpudirect" \
       "-storage/cuobject/"
  echo ""
fi

# ── 5. Verify ───────────────────────────────

info "Verifying installation..."

echo ""
echo "--- Installed cuObject packages ---"
dpkg -l | grep cuobj || true

echo ""
echo "--- Libraries in linker cache ---"
ldconfig -p | grep cuobj || true

echo ""
info "Done.  Re-run CMake with:"
echo "  cmake .. -DHIPOBJ_CUOBJECT_FROM_TOOLKIT=ON"
