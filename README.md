# hipObject

[![License](https://img.shields.io/badge/License-MIT-blue.svg)][license]
[![Build](https://img.shields.io/github/actions/workflow/status/ROCm/hipObject/hipobject-build.yml?label=Build)][ci-build]
[![Coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FROCm%2FhipObject%2Fbadges%2Fcoverage.json)][ci-build]
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
- S3 control plane with RDMA data plane split: your S3 client makes
  the requests, and hipObject provides the RDMA side
- Automatic NIC selection (the NIC closest to the GPU, by PCI bus ID)
- Supports Broadcom Thor-2 (`bnxt_re`) and AMD Pensando
  ionic (`ionic_rdma`) RDMA NICs
- dmabuf-based GPU memory export for RDMA registration
- Host-staged fallback when the NIC can't register GPU memory
- Host memory buffers, including on hosts without a GPU
- Thread-safe API, with concurrent transfers
- Wire-compatible with cuObject `x-amz-rdma-token` protocol

## Transfers

hipObject doesn't make S3 requests. Your S3 client sends each request,
with an RDMA token from hipObject in its `x-amz-rdma-token` header, and
the server moves the data by RDMA:

| Call          | Your S3 request                                | The server                    |
| ------------- | ---------------------------------------------- | ----------------------------- |
| `hipObjGet()` | GetObject, for a whole object or a byte range  | RDMA WRITEs into your buffer  |
| `hipObjPut()` | PutObject, or UploadPart of a multipart upload | RDMA READs from your buffer   |

The request selects the object, and the part of it, to transfer; the
token only describes your buffer.

## Requirements

### Software

- Linux (Ubuntu 24.04+, RHEL 9+)
- ROCm 7.14+ (HIP runtime, HSA runtime)
- libibverbs and the NIC's libibverbs provider, from rdma-core
  (`libibverbs1` and `ibverbs-providers` on Ubuntu, `libibverbs` on RHEL)
- Linux kernel 6.18+ (for `ionic_rdma` driver)
- CMake 3.25.2+
- C++20 compiler: GCC 13+, or Clang (e.g., amdclang++) with GCC 13+'s
  libstdc++ (on RHEL 9, `gcc-toolset-13` or later)
- Optional: libcurl's development files, for the examples' `--live` mode

### Hardware

- AMD Instinct GPU (MI200 / MI300 series), for transfers to and from
  GPU memory
- Broadcom Thor-2 or AMD Pensando Pollara 400 NIC
- RoCEv2-capable network fabric with PFC/ECN
- An S3 server that supports RDMA transfers (see
  [docs/interop.rst](docs/interop.rst))

## Building

See [INSTALL.md](INSTALL.md) for detailed build instructions.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The tests don't need a GPU, an RDMA NIC, or an S3 server. hipObject
installs into the ROCm installation (`ROCM_PATH`) by default.

## Quick Start

This program reads an object into GPU memory. The two callbacks are
where your S3 client goes; until you fill them in, `hipObjGet()` fails
with `hipObjS3Error`. The programs in [`examples/`](examples/) fill
them in with libcurl.

```c
#include <stdio.h>

#include <hip/hip_runtime_api.h>
#include <hipobj.h>

#define BUF_SIZE ((size_t)64 * 1024 * 1024)

/* hipObject doesn't make S3 requests: your S3 client does, in these two
 * callbacks. ctx is the pointer passed to hipObjGet(). */
static int
sendRequest(void *ctx, const char *token, size_t tokenLen)
{
    /* Send the S3 request, such as GET /bucket/object, with the tokenLen
     * characters at token as its x-amz-rdma-token header, and wait for
     * the response. Return 0 on success. */
    (void)ctx;
    (void)token;
    (void)tokenLen;
    return -1;
}

static int
recvReply(void *ctx, char *reply, size_t *replyLen)
{
    /* Copy the response's x-amz-rdma-reply header into reply, which has
     * room for *replyLen characters, and set *replyLen to its length.
     * Return 0 on success. */
    (void)ctx;
    (void)reply;
    (void)replyLen;
    return -1;
}

int
main(void)
{
    /* Use GPU 0 and the RDMA NIC closest to it */
    hipObjConfig_t config = {.gpuDevice = 0, .nicHint = NULL};
    hipObjError_t  err    = hipObjInit(&config);
    if (err.opError != hipObjSuccess) {
        fprintf(stderr, "hipObjInit: %s\n", hipObjGetErrorString(err.opError));
        return 1;
    }

    void *gpuBuf = NULL;
    if (hipMalloc(&gpuBuf, BUF_SIZE) != hipSuccess) {
        fprintf(stderr, "hipMalloc failed\n");
        hipObjShutdown();
        return 1;
    }

    err = hipObjBufRegister(gpuBuf, BUF_SIZE);
    if (err.opError == hipObjSuccess) {
        /* Read BUF_SIZE bytes into the start of gpuBuf. The handle isn't
         * used, and ctx (here NULL) is passed to the callbacks. */
        hipObjOps_t ops = {.sendRequest = sendRequest, .recvReply = recvReply};
        err             = hipObjGet(NULL, gpuBuf, BUF_SIZE, 0, &ops, NULL);

        /* If a copy through host memory timed out, it can still write
         * gpuBuf, and deregistering returns hipObjBusy until it's done.
         * Don't free memory that's still in use: leave it to the end of
         * the process. */
        if (hipObjBufDeregister(gpuBuf).opError != hipObjSuccess) {
            gpuBuf = NULL;
        }
    }
    if (err.opError != hipObjSuccess) {
        fprintf(stderr, "hipObject: %s\n", hipObjGetErrorString(err.opError));
    }

    if (gpuBuf != NULL) {
        (void)hipFree(gpuBuf);
    }
    hipObjShutdown();
    return err.opError == hipObjSuccess ? 0 : 1;
}
```

Build it with CMake, against the installed hipObject:

```cmake
cmake_minimum_required(VERSION 3.25.2)
project(quickstart LANGUAGES C)

find_package(hip CONFIG REQUIRED)
find_package(hipobj CONFIG REQUIRED)

add_executable(quickstart quickstart.c)
target_link_libraries(quickstart PRIVATE hipobj::hipobj hip::host)
```

Configure it with `-DCMAKE_PREFIX_PATH=/opt/rocm`, or wherever ROCm and
hipObject are installed. The header is installed as
`include/hipobj/hipobj.h`, and `hipobj::hipobj` adds its directory to
the include path. The program uses HIP itself, so it links `hip::host`
too.

For transfers to and from host memory, allocate a host buffer (for
example with `malloc()` or `hipHostMalloc()`) and register it with
`hipObjBufRegisterHost()`. On a host without a GPU, set
`config.gpuDevice` to -1 and `config.nicHint` to the RDMA device's name
(as `ibv_devices` lists it), so `hipObjInit()` opens that NIC without
looking for a GPU.

## Architecture

hipObject separates control and data planes:

- **Control plane**: Standard S3 REST requests augmented
  with `x-amz-rdma-token` / `x-amz-rdma-reply` headers,
  made by the application's S3 client
- **Data plane**: RDMA READ/WRITE over RoCEv2 RC transport
  directly between GPU VRAM and storage server buffers

The library uses RC (Reliable Connection) transport rather
than DC (Dynamic Connection), since DC is exclusive to
Mellanox/NVIDIA ConnectX hardware. A server-side [adapter](https://github.com/versity/versitygw/tree/main/cuwrapper)
bridges RC clients to cuObject's DC-based server library.

See [docs/architecture.rst](docs/architecture.rst) for how the
library works, and [docs/interop.rst](docs/interop.rst) for the
cuObject v1.2.0 compatibility matrix and testing guide.

## Testing

- **Unit tests**: `ctest --test-dir build` runs them all, without a
  GPU, an RDMA NIC, or an S3 server
- **RC test server**: `hipobj-rdma-test-server`, an S3 server that
  moves data by RDMA (see [docs/interop.rst](docs/interop.rst))
- **Examples**: `get-object` and `put-object`, which need a GPU and an
  RDMA NIC; with libcurl, `get-object 4096 --live http://host:9000`
  runs against a server (see [examples/README.md](examples/README.md))
- **MinIO bridge**: `-DHIPOBJ_MINIO_CLIENT=ON` (see
  [integrations/minio-cpp/TESTING.md](integrations/minio-cpp/TESTING.md))
- **Emulated hardware**: the `HW:` workflows above run the RDMA data
  path on rocm-ernic's emulated ionic NIC and rocjitsu's emulated GPU,
  under QEMU

## Documentation

Full documentation is published at
<https://rocm.github.io/hipObject/>. Its source lives in the
[`docs/`](docs/) directory and covers building, the API
reference, architecture, and cuObject interoperability.

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
