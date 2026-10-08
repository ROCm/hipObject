# hipObject

[![License](https://img.shields.io/badge/License-MIT-blue.svg)][license]
[![Build](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-build.yml?label=Build)][ci-build]
[![Docs](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-documentation-check.yml?label=Docs)][ci-docs]
[![clang-format](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-format-check.yml?label=clang-format)][ci-clang-format]
[![ShellCheck](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-shellcheck.yml?label=ShellCheck)][ci-shellcheck]
[![pylint](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-pylint.yml?label=pylint)][ci-pylint]
[![cmakelint](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-cmakelint.yml?label=cmakelint)][ci-cmakelint]
[![codespell](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-spell-check.yml?label=codespell)][ci-codespell]
[![Ansible](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-ansible.yml?label=Ansible)][ci-ansible]
[![CodeQL](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/codeql.yml?label=CodeQL)][ci-codeql]
[![MinIO Integration](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-minio-integration-check.yml?label=MinIO%20Integration)][ci-minio-integration]
[![HW: AIS and Host Buffers](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-hardware-test-gpu-direct.yml?label=HW%3A%20AIS%20and%20Host%20Buffers)][ci-hw-gpu-direct]
[![HW: Two-VM AIS and Host Buffers](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-hardware-test-two-vm-gpu-direct.yml?label=HW%3A%20Two-VM%20AIS%20and%20Host%20Buffers)][ci-hw-two-vm-gpu-direct]
[![HW: Two-VM Protocol](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-hardware-test-two-vm-protocol.yml?label=HW%3A%20Two-VM%20Protocol)][ci-hw-two-vm-protocol]
[![HW: S3 Backend](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-hardware-test-s3-backend.yml?label=HW%3A%20S3%20Backend)][ci-hw-s3-backend]
[![HW: MinIO Bridge](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-hardware-test-minio-bridge.yml?label=HW%3A%20MinIO%20Bridge)][ci-hw-minio-bridge]
[![Platform](https://img.shields.io/badge/platform-linux-lightgrey.svg)](INSTALL.md)
[![ROCm](https://img.shields.io/badge/ROCm-supported-green.svg)](https://rocm.docs.amd.com)
![Language](https://img.shields.io/badge/language-C%20%7C%20C%2B%2B-orange.svg)

> [!CAUTION]
> This release is an *early-access* software technology
> preview. Running production workloads is *not*
> recommended.

RDMA-accelerated S3 object storage client for AMD GPUs.

hipObject enables direct data transfers between AMD GPU VRAM
and S3-compatible object storage using RDMA over RoCEv2. It
interoperates with NVIDIA cuObject-equipped storage servers
via the `x-amz-rdma-token` S3 header protocol, providing a
vendor-neutral client for GPU-direct object storage.

## Features

- Direct GPU VRAM to/from S3 object storage via RDMA
- Zero-copy data path bypassing host CPU for payloads
- S3 control plane with RDMA data plane split
- NUMA-aware NIC selection (closest NIC to target GPU)
- Supports Broadcom Thor-2 (`bnxt_re`) and AMD Pensando
  ionic (`ionic_rdma`) RDMA NICs
- dmabuf-based GPU memory export for RDMA registration
- Host-staged fallback when dmabuf is unavailable
- Wire-compatible with cuObject `x-amz-rdma-token` protocol

## Supported S3 Operations

| Operation    | Description                      |
| ------------ | -------------------------------- |
| GET          | Fetch object to GPU VRAM         |
| PUT          | Store GPU VRAM to object         |
| UPLOAD_PART  | Chunked upload (multipart)       |
| RANGE_GET    | Byte-range fetch from object     |

## Requirements

### Software

- Linux (Ubuntu 22.04+, RHEL 9+)
- ROCm 6.x+ (HIP runtime, HSA runtime)
- Linux kernel 6.18+ (for `ionic_rdma` driver)
- CMake 3.21+
- C++20 compiler (e.g., g++ or amdclang++)

### Hardware

- AMD Instinct GPU (MI200 / MI300 series)
- Broadcom Thor-2 or AMD Pensando Pollara 400 NIC
- RoCEv2-capable network fabric with PFC/ECN

## Building

See [INSTALL.md](INSTALL.md) for detailed build instructions.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Quick Start

```c
#include <hipobj.h>
#include <hip/hip_runtime.h>

void* gpu_buf;
hipMalloc(&gpu_buf, 64 * 1024 * 1024);

hipObjConfig_t config = {
  .endpoint = "https://s3.example.com",
  .region   = "us-east-1",
};
hipObjInit(&config);
hipObjBufRegister(gpu_buf, 64 * 1024 * 1024);

hipObjHandle_t handle = /* from S3 SDK */;
hipObjGet(handle, gpu_buf, 64 * 1024 * 1024, 0);

hipObjBufDeregister(gpu_buf);
hipObjShutdown();
hipFree(gpu_buf);
```

For CPU-based transfers, allocate a host buffer (for example with `malloc()` or
`hipHostMalloc()`) and register it with `hipObjBufRegisterHost()`. On GPU-less
hosts, set `config.nicHint` to the RDMA device name so `hipObjInit()` can open
that NIC without GPU topology discovery.

## Architecture

hipObject separates control and data planes:

- **Control plane**: Standard S3 REST requests augmented
  with `x-amz-rdma-token` / `x-amz-rdma-reply` headers
- **Data plane**: RDMA READ/WRITE over RoCEv2 RC transport
  directly between GPU VRAM and storage server buffers

The library uses RC (Reliable Connection) transport rather
than DC (Dynamic Connection), since DC is exclusive to
Mellanox/NVIDIA ConnectX hardware. A server-side [adapter](https://github.com/versity/versitygw/tree/main/cuwrapper)
bridges RC clients to cuObject's DC-based server library.

See [docs/interop.rst](docs/interop.rst) for the cuObject
v1.2.0 compatibility matrix and testing guide.

## Testing

- **Unit tests**: `ctest -R test-rdma-token` (no hardware)
- **RC test server**: `hipobj-rdma-test-server` (see interop doc)
- **Live examples**: `get-object --live http://host:9000` with libcurl
- **MinIO bridge**: `-DHIPOBJ_MINIO_CLIENT=ON` (see
  `integrations/minio-cpp/TESTING.md`)

## Documentation

Full documentation is published at
<https://rocm.github.io/hipObject/>. Its source lives in the
[`docs/`](docs/) directory and covers building, the API
reference, and architecture.

## License

MIT. See [LICENSE.md](LICENSE.md).

<!-- References -->

[license]: https://github.com/ROCm/hipObject/blob/develop/LICENSE.md
[ci-build]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-build.yml
[ci-docs]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-documentation-check.yml
[ci-clang-format]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-format-check.yml
[ci-shellcheck]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-shellcheck.yml
[ci-pylint]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-pylint.yml
[ci-cmakelint]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-cmakelint.yml
[ci-codespell]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-spell-check.yml
[ci-ansible]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-ansible.yml
[ci-codeql]: https://github.com/ROCm/hipObject/actions/workflows/codeql.yml
[ci-minio-integration]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-minio-integration-check.yml
[ci-hw-gpu-direct]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-hardware-test-gpu-direct.yml
[ci-hw-two-vm-gpu-direct]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-hardware-test-two-vm-gpu-direct.yml
[ci-hw-two-vm-protocol]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-hardware-test-two-vm-protocol.yml
[ci-hw-s3-backend]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-hardware-test-s3-backend.yml
[ci-hw-minio-bridge]: https://github.com/ROCm/hipObject/actions/workflows/hipobject-hardware-test-minio-bridge.yml
