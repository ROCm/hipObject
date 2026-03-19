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
- Doxygen (for documentation)

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

| Option             | Default | Description                |
| ------------------ | ------- | -------------------------- |
| `HIPOBJ_BNXT`      | ON      | Build Thor-2 RDMA backend  |
| `HIPOBJ_IONIC`     | OFF     | Build ionic RDMA backend   |
| `BUILD_SHARED_LIBS`| OFF     | Build shared library       |
| `BUILD_TESTING`    | ON      | Build and register tests   |
| `ROCM_PATH`        | /opt/rocm | Path to ROCm install     |

## Environment Variables

- `ROCM_PATH`: Override the ROCm installation path
- `LD_LIBRARY_PATH`: Must include ROCm and rdma-core
  library paths at runtime
