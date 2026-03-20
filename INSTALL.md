# Building hipObject

## Prerequisites

### Required

- CMake 3.21 or later
- ROCm 6.x or later (provides HIP, HSA runtime)
- C++17 capable compiler (hipcc from ROCm)

### Optional

- rdma-core development headers (for system libibverbs;
  hipObject loads libibverbs via dlopen so this is not
  strictly required at build time)
- GTest (fetched automatically if not found)
- Doxygen, Python 3, Sphinx, Breathe (for documentation)

### cuObject (NVIDIA Interoperability)

hipObject interoperates with NVIDIA
[cuObject](https://docs.nvidia.com/gpudirect-storage/cuobject/)
storage servers via the `x-amz-rdma-token` S3 header
protocol. The cuObject client and server libraries are
optional and can be obtained in two ways.

#### Option A: CUDA Toolkit apt repository (Ubuntu)

This is the recommended method when running on a system
with an NVIDIA CUDA installation. It follows the
[CUDA Installation Guide for Linux (Ubuntu)](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html#ubuntu-installation).

1. Install the NVIDIA repository keyring (if not already
   present):

```bash
DISTRO=ubuntu2404  # adjust for your release
ARCH=x86_64        # or sbsa for arm64
wget https://developer.download.nvidia.com/\
compute/cuda/repos/${DISTRO}/${ARCH}/\
cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
```

2. Install the cuObject packages:

```bash
sudo apt update
sudo apt install cuda-toolkit          # client
sudo apt install libcuobjserver \
                 libcuobjserver-dev    # server
```

3. Configure hipObject to find the toolkit:

```bash
cmake .. -DHIPOBJ_CUOBJECT_FROM_TOOLKIT=ON
```

A convenience script that automates steps 1-2 is
provided:

```bash
sudo ./scripts/setup-cuobject-apt.sh
```

#### Option B: CDN tarball download (client only)

The client library can also be downloaded directly from
the NVIDIA CUDA redistributable CDN without configuring
the apt repository:

```bash
cmake .. -DHIPOBJ_FETCH_CUOBJECT_CLIENT=ON
```

The server library is not available on the public CDN
and must be installed separately. Use
`-DHIPOBJ_FIND_CUOBJECT_SERVER=ON` with
`-DCUOBJSERVER_ROOT=/path/to/install` to locate a
manually installed server library.

## Build Steps

```bash
# Configure
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DHIPOBJ_BNXT=ON \
  -DHIPOBJ_IONIC=OFF

# Build
make -j$(nproc)

# Run tests (requires GPU + RDMA NIC)
ctest --output-on-failure

# Install (to ROCM_PATH, default /opt/rocm)
sudo make install
```

## CMake Options

| Option | Default | Description |
| ------ | ------- | ----------- |
| `HIPOBJ_BNXT` | ON | Build Thor-2 RDMA backend |
| `HIPOBJ_IONIC` | OFF | Build ionic RDMA backend |
| `BUILD_SHARED_LIBS` | OFF | Build shared library |
| `BUILD_TESTING` | ON | Build and register tests |
| `HIPOBJ_BUILD_DOCS` | OFF | Build documentation |
| `HIPOBJ_DOCS_ONLY` | OFF | Docs targets only |
| `ROCM_PATH` | /opt/rocm | Path to ROCm install |
| `HIPOBJ_CUOBJECT_FROM_TOOLKIT` | OFF | Find cuObject in CUDA toolkit |
| `HIPOBJ_FETCH_CUOBJECT_CLIENT` | OFF | Download client from CDN |
| `HIPOBJ_FIND_CUOBJECT_SERVER` | OFF | Find system cuObject server |
| `CUOBJSERVER_ROOT` | (empty) | cuObject server prefix |

## Building Documentation

To build the HTML documentation locally:

```bash
cmake -B build -DHIPOBJ_BUILD_DOCS=ON
cmake --build build --target sphinx-html
```

The output appears in `build/docs/html/`.

For a docs-only build that does not require a ROCm/HIP
toolchain:

```bash
cmake -B build \
  -DHIPOBJ_DOCS_ONLY=ON \
  -DHIPOBJ_BUILD_DOCS=ON
cmake --build build --target sphinx-html
```

## Environment Variables

- `ROCM_PATH`: Override the ROCm installation path
- `LD_LIBRARY_PATH`: Must include ROCm and rdma-core
  library paths at runtime
