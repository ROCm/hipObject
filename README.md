# hipObject

[![MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE.md)
[![Build](https://github.com/ROCm/hipObject/actions/workflows/hipobject-build.yml/badge.svg)](https://github.com/ROCm/hipObject/actions/workflows/hipobject-build.yml)
[![Docs](https://github.com/ROCm/hipObject/actions/workflows/hipobject-docs.yml/badge.svg)](https://github.com/ROCm/hipObject/actions/workflows/hipobject-docs.yml)
[![clang-format](https://github.com/ROCm/hipObject/actions/workflows/hipobject-clang-format.yml/badge.svg)](https://github.com/ROCm/hipObject/actions/workflows/hipobject-clang-format.yml)
[![codespell](https://github.com/ROCm/hipObject/actions/workflows/hipobject-codespell.yml/badge.svg)](https://github.com/ROCm/hipObject/actions/workflows/hipobject-codespell.yml)
[![Ansible](https://github.com/ROCm/hipObject/actions/workflows/hipobject-ansible.yml/badge.svg)](https://github.com/ROCm/hipObject/actions/workflows/hipobject-ansible.yml)
[![Platform](https://img.shields.io/badge/platform-linux-lightgrey.svg)](INSTALL.md)
[![ROCm](https://img.shields.io/badge/ROCm-supported-green.svg)](https://rocm.docs.amd.com)
[![Language](https://img.shields.io/badge/language-HIP%20%7C%20C-orange.svg)](https://rocm.docs.amd.com/projects/HIP/en/latest/)

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
- C++17 compiler (hipcc)

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

## Architecture

hipObject separates control and data planes:

- **Control plane**: Standard S3 REST requests augmented
  with `x-amz-rdma-token` / `x-amz-rdma-reply` headers
- **Data plane**: RDMA READ/WRITE over RoCEv2 RC transport
  directly between GPU VRAM and storage server buffers

The library uses RC (Reliable Connection) transport rather
than DC (Dynamic Connection), since DC is exclusive to
Mellanox/NVIDIA ConnectX hardware. A server-side adapter
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

Full documentation lives in the [`docs/`](docs/) directory
and covers building, the API reference, and architecture.

## License

MIT. See [LICENSE.md](LICENSE.md).
