# Changelog for hipObject

## (Unreleased) hipObject 0.1.0

### Added

### Changed

- hipObject is now built as plain C/C++ instead of HIP. It contains no GPU
  kernels, so the HIP language (and its per-architecture device compilation
  pass) is no longer enabled. The HIP runtime is still used via the
  `hip::host` CMake target. `CMAKE_HIP_COMPILER` no longer needs to be set
  when configuring.
- hipObject is now built as C++20 (without compiler extensions) instead of
  C++17, so a C++20-capable compiler is required. The
  `HIPOBJ_CXX_STANDARD` CMake cache variable now defaults to `20`.
- hipObject is now built as a shared library (`libhipobj.so`) by default.
  The `BUILD_SHARED_LIBS` CMake option, which was previously ignored, now
  selects between a shared and a static library and defaults to `ON`. Set
  it to `OFF` to build the static library (`libhipobj.a`) as before.
  The shared library exports only the public C API declared in
  `hipobj.h`.
- The `hipobj::hipobj` CMake target no longer passes `hip::host` and
  `hsa-runtime64::hsa-runtime64` on to consumers, since `hipobj.h`
  doesn't include HIP or HSA headers. Consumers that use HIP themselves
  must link `hip::host` directly.

### Removed

- The `OFFLOAD_ARCH` CMake cache variable and its `rocminfo`-based GPU
  architecture detection. It was never used to set the target architecture
  and is unnecessary now that nothing is compiled for the GPU.
- The `hipObjInitV2()` declaration in `hipobj.h`. The function was never
  implemented, so calls to it failed at link time. They now fail to
  compile.

### Known issues

