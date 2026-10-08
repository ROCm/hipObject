# Changelog for hipObject

## (Unreleased) hipObject 0.1.0

### Added

- hipObject is installed and packaged with rocm-cmake. `cpack` builds
  `hipobject` (runtime) and `hipobject-dev` (development) DEB packages
  or `hipobject` and `hipobject-devel` RPM packages, or a single
  `hipobject-static-dev` DEB or `hipobject-static-devel` RPM package
  for a static build. rocm-cmake is downloaded if ROCm doesn't provide
  it.
- The `ROCM_VERSION` CMake and environment variable sets the ROCm
  version, which selects the package dependencies and the default
  `ROCM_PATH`. It defaults to the version of the ROCm in `ROCM_PATH`.
- hipObject can be built as part of another CMake project with
  `add_subdirectory()`. When it is, it leaves the build type and install
  prefix to the parent project instead of defaulting them to
  `RelWithDebInfo` and `ROCM_PATH`.

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
- The public API functions check their arguments more thoroughly, and
  return `hipObjInvalidValue` for arguments they used to accept or
  report as a different error:
  - `hipObjInit()` rejects a nonzero `flags` (it's reserved), a
    `gpuDevice` below -1, and a `gpuDevice` that names a GPU that
    doesn't exist.
  - `hipObjBufRegister()` and `hipObjBufRegisterHost()` reject a NULL
    pointer, a size of 0, and a buffer that would wrap past the end of the
    address space. `hipObjBufDeregister()` rejects a NULL pointer.
  - `hipObjGet()`, `hipObjPut()`, `hipObjBufSync()`, and
    `hipObjGetRdmaToken()` reject a NULL pointer, a size of 0, a negative
    offset, and a range that doesn't fit inside the registered buffer.
    `hipObjGet()` and `hipObjPut()` used to report a range outside the
    buffer as `hipObjRdmaError`, as did `hipObjGetRdmaToken()` for a size
    larger than the buffer, and `hipObjBufSync()` used to accept one for
    a buffer that wasn't staged through host memory.
  - `hipObjGet()` and `hipObjPut()` reject a `hipObjOps_t` without both
    callbacks before calling either of them. They used to return
    `hipObjS3Error`.
  - `hipObjParseRdmaReply()` and `hipObjTokenClientNic()` accept only the
    forms that `docs/interop.rst` lists. An `x-amz-rdma-reply` value must
    be `ok`, `err`, a three-digit HTTP status, or `200:` followed by an
    88-hex-digit token; prefixes such as `okay`, signs, whitespace,
    malformed tokens, and tokens after other statuses used to be accepted.
    A token whose transport byte is neither DC (`00`) nor RC (`01`) is
    rejected.
  - `hipObjGetV2()` and `hipObjPutV2()`, which aren't implemented yet,
    check their arguments before returning `hipObjNotSupported`. They
    reject a NULL or empty bucket or key, a bucket longer than 255
    characters or a key longer than 1024 bytes, a NULL pointer, a size of
    0, an offset and size whose sum wraps, a query string that isn't
    canonical, and a `hipObjOpsV2_t` without all three callbacks. A
    canonical query is a list of `key=value` parameters, sorted by key and
    then by value, in which every character except the RFC 3986
    unreserved ones is percent-encoded with uppercase hex digits. A size
    above 2^31-1 bytes returns `hipObjSizeTooLarge`.
- The `hipobj.h` documentation of `hipObjGet()` and `hipObjPut()` now
  says that `offset` is a byte offset into the registered buffer. It
  used to describe it as an offset into the S3 object, but it has always
  been used as a buffer offset. The request your callbacks make selects
  the part of the object to transfer.

### Removed

- The `OFFLOAD_ARCH` CMake cache variable and its `rocminfo`-based GPU
  architecture detection. It was never used to set the target architecture
  and is unnecessary now that nothing is compiled for the GPU.
- The `hipObjInitV2()` declaration in `hipobj.h`. The function was never
  implemented, so calls to it failed at link time. They now fail to
  compile.
- The `HIPOBJ_SUCCESS` macro from `hipobj.h`. It expanded to a compound
  literal, which isn't valid C++. Compare a `hipObjError_t`'s `opError`
  with `hipObjSuccess` instead.

### Resolved issues

- `hipObjGet()`, `hipObjPut()`, and `hipObjBufSync()` could accept an
  offset and size whose sum wrapped around, and advertise a range outside
  the registered buffer to the server or copy outside it.

### Known issues

