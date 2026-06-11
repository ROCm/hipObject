# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#

# track: rocm/dev-ubuntu-24.04:7.2
# Digest from Docker Hub tag 7.2 (amd64). Bump with:
#   docker buildx imagetools inspect rocm/dev-ubuntu-24.04:7.2 --format '{{json .}}'
# Re-pin apt versions after changing the base digest (see AGENTS.md).
FROM rocm/dev-ubuntu-24.04@sha256:749f9ee120c739682cc2e1553e62632c2676f98bc49e4d8133f380e0af682bcc

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake=3.28.3-1build7 \
        git=1:2.43.0-1ubuntu7.3 \
        libibverbs-dev=50.0-2ubuntu0.2 \
        libnuma-dev=2.0.18-1ubuntu0.24.04.1 \
        libssl-dev=3.0.13-0ubuntu3.11 \
        zlib1g-dev=1:1.3.dfsg-3.1ubuntu2.1 \
        libcurl4-openssl-dev=8.5.0-2ubuntu10.9 \
    && rm -rf /var/lib/apt/lists/*
