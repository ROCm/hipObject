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

| Option             | Default   | Description                |
| ------------------ | --------- | -------------------------- |
| `HIPOBJ_BNXT`      | ON        | Build Thor-2 RDMA backend  |
| `HIPOBJ_IONIC`     | OFF       | Build ionic RDMA backend   |
| `BUILD_SHARED_LIBS`| OFF       | Build shared library       |
| `BUILD_TESTING`    | ON        | Build and register tests   |
| `HIPOBJ_BUILD_DOCS`| OFF       | Build documentation        |
| `HIPOBJ_DOCS_ONLY` | OFF       | Configure docs targets only|
| `HIPOBJ_MINIO_CLIENT` | OFF    | Build minio-cpp RDMA bridge|
| `HIPOBJ_INTEGRATION_TESTS` | ON | Build RC test server       |
| `HIPOBJ_FETCH_CUOBJECT_CLIENT` | OFF | Fetch libcuobjclient 1.2.0.59 |
| `HIPOBJ_FIND_CUOBJECT_SERVER` | OFF | Find libcuobjserver + probe |
| `ROCM_PATH`        | /opt/rocm | Path to ROCm install       |

## MinIO C++ RDMA bridge (`HIPOBJ_MINIO_CLIENT`)

Build the `hipobj_minio` library and `minio-getput-rdma` example:

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHIPOBJ_MINIO_CLIENT=ON
cmake --build build
```

Additional system packages (Ubuntu 24.04):

```bash
sudo apt install libssl-dev zlib1g-dev libcurl4-openssl-dev
```

See [integrations/minio-cpp/TESTING.md](integrations/minio-cpp/TESTING.md)
for lab validation against MinIO AIStor over RDMA.

## cuObject interoperability

See [docs/interop.rst](docs/interop.rst) for the v1.2.0 compatibility
matrix.  Build the in-repo RC test server:

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build --target hipobj-rdma-test-server
```

Optional cuObject library probes:

```bash
cmake -B build \
  -DHIPOBJ_FETCH_CUOBJECT_CLIENT=ON \
  -DHIPOBJ_FIND_CUOBJECT_SERVER=ON \
  -DCUOBJSERVER_ROOT=/opt/nvidia/cuobjserver
```

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
