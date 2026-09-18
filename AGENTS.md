# AGENTS.md

Guidance for AI coding agents and Cursor Cloud agents working in the hipObject
repository. hipObject is an RDMA-accelerated S3 object client for AMD GPUs; see
[README.md](README.md) for scope and [INSTALL.md](INSTALL.md) for full CMake
options.

## Toolchain and paths

- **ROCm**: `ROCM_PATH` defaults to `/opt/rocm`. Put `/opt/rocm/bin` on `PATH`
  and include `/opt/rocm/lib` in `LD_LIBRARY_PATH` when running binaries you
  build.
- **HIP compiler**: CI and the cloud image configure CMake with
  `-DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/clang` (see
  [.github/workflows/build-check.yml](.github/workflows/build-check.yml)).

## Configure and build (default agent flow)

From the repository root, match CI:

```bash
cmake -B build \
  -DCMAKE_PREFIX_PATH=/opt/rocm \
  -DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/clang \
  -DBUILD_TESTING=ON \
  -DHIPOBJ_BNXT=ON \
  -DHIPOBJ_IONIC=ON
cmake --build build -j"$(nproc)"
```

Notes:

- [INSTALL.md](INSTALL.md) defaults `HIPOBJ_IONIC` to **OFF** for local lab
  workflows; CI and Cursor Cloud use **ON** to compile both RDMA backends—keep
  that difference in mind when debugging.
- Optional MinIO C++ bridge: `-DHIPOBJ_MINIO_CLIENT=ON` plus the dependencies
  already installed in [`.cursor/Dockerfile`](.cursor/Dockerfile); see
  [integrations/minio-cpp/TESTING.md](integrations/minio-cpp/TESTING.md).

## Tests

- After a successful build: `cd build && ctest --output-on-failure`.
- Many integration paths need **GPU hardware**, **RDMA NICs**, and a compatible
  S3/cuObject lab; see [README.md](README.md) and [docs/interop.rst](docs/interop.rst).
  Cloud agents and plain CPU containers may pass only a subset of tests.

## Cursor Cloud specific instructions

Cloud agents use [`.cursor/environment.json`](.cursor/environment.json):

- **Image**: Built from [`.cursor/Dockerfile`](.cursor/Dockerfile), which extends
  a **digest-pinned** `rocm/dev-ubuntu-24.04` image (see the `# track: …` line
  for the logical Docker Hub tag) and installs extra packages with **exact apt
  versions** for reproducibility.
- **`install`**: Runs from the repo root and performs configure + compile only
  (same CMake flags as CI). It does **not** run `ctest` so cold starts stay
  predictable when no GPU is present.
- **Secrets** (S3 keys, lab endpoints): configure in the Cursor Cloud Agents
  dashboard, not in committed files.

## Maintaining pinned toolchain

**What is pinned**

1. **Base OCI image**: `FROM …@sha256:…` in [`.cursor/Dockerfile`](.cursor/Dockerfile),
   with `# track: rocm/dev-ubuntu-24.04:<tag>` naming the Docker Hub tag used
   when the digest was recorded.
2. **APT packages**: `package=version` pins in the same Dockerfile, captured for
   Ubuntu 24.04 (main + updates) so `apt-get install` stays stable across mirror
   drift.

**Check whether the base digest is stale**

From any directory (the script resolves the repo root from its path):

```bash
bash .cursor/scripts/check-cursor-rocm-image-pin.sh
```

It prefers `docker buildx imagetools inspect`, then `skopeo`, then the Docker Hub
HTTP API. Exit **0** when the pinned digest still matches the tracked tag; exit
**1** when Docker Hub serves a different digest.

**Refresh pins after an update**

1. Pick the tag you want to follow (must match `# track:`), e.g.
   `rocm/dev-ubuntu-24.04:7.2`.
2. Resolve the digest, for example:

```bash
docker buildx imagetools inspect rocm/dev-ubuntu-24.04:7.2 \
  --format '{{.Manifest.Digest}}'
```

3. Update the `FROM …@sha256:…` line in [`.cursor/Dockerfile`](.cursor/Dockerfile).
4. Re-pin apt versions by running `apt-get update` and `apt-cache madison`
   **inside** the new base image digest for the packages listed in the
   Dockerfile, then update the `RUN apt-get install` line.

Optional CI: schedule `bash .cursor/scripts/check-cursor-rocm-image-pin.sh`
weekly with `continue-on-error: true` for notifications, or fail the job to
force a deliberate pin bump.

## Lab provisioning (not Cloud)

For GPU hosts, ROCm, RDMA, and MinIO AIStor installs, use
[ansible/README.md](ansible/README.md). That path is out of scope for the Cursor
Cloud image.
