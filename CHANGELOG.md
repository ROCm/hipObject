# Changelog for hipObject

## (Unreleased) hipObject 0.1.0

### Added

- hipObject is installed and packaged with rocm-cmake. `cpack` builds
  `hipobject` (runtime) and `hipobject-dev` (development) DEB packages
  or `hipobject` and `hipobject-devel` RPM packages, or a single
  `hipobject-static-dev` DEB or `hipobject-static-devel` RPM package
  for a static build. rocm-cmake is downloaded if ROCm doesn't provide
  it.
- Nightly DEB packages of the `develop` branch, for Ubuntu 24.04 with
  ROCm 10.1.0 and 7.14.1, are published as the `nightly`
  prerelease on GitHub, which each new build replaces. They're for
  testing, and aren't a supported release.
- The `ROCM_VERSION` CMake and environment variable sets the ROCm
  version, which selects the package dependencies and the default
  `ROCM_PATH`. It defaults to the version of the ROCm in `ROCM_PATH`.
- hipObject can be built as part of another CMake project with
  `add_subdirectory()`. When it is, it leaves the build type and install
  prefix to the parent project instead of defaulting them to
  `RelWithDebInfo` and `ROCM_PATH`.
- The `HIPOBJ_WERROR` CMake option makes the build treat compiler
  warnings in hipObject's own code as errors. It defaults to `OFF`.
- The `HIPOBJ_USE_IWYU` CMake option runs include-what-you-use on
  hipObject's own code while it compiles, which prints suggestions for
  `#include` lines to add or remove. The suggestions don't fail the
  build. It defaults to `OFF`.
- The `HIPOBJ_USE_CODE_COVERAGE` CMake option builds hipObject with
  LLVM code coverage instrumentation, and adds a `hipobj-coverage`
  target that reports the library's coverage after the tests run. It
  needs Clang and defaults to `OFF`.
- The `HIPOBJ_CXX_STANDARD` CMake cache variable accepts `26`, which
  builds hipObject as C++26. It needs CMake 3.30 or later, and
  configuring fails with an earlier one.
- With `CMAKE_EXPORT_COMPILE_COMMANDS` on, `compile_commands.json`
  lists the compiler's implicit include directories, so tools that read
  it, such as clangd, find the same system headers as the compiler.

### Changed

- hipObject is now built as plain C/C++ instead of HIP. It contains no GPU
  kernels, so the HIP language (and its per-architecture device compilation
  pass) is no longer enabled. The HIP runtime is still used via the
  `hip::host` CMake target. `CMAKE_HIP_COMPILER` no longer needs to be set
  when configuring.
- hipObject is now built as C++20 (without compiler extensions) instead of
  C++17, so a C++20-capable compiler is required. The
  `HIPOBJ_CXX_STANDARD` CMake cache variable now defaults to `20`.
- The `HIPOBJ_USE_CLANG_TIDY` CMake option now requires Clang as the
  C++ compiler, and configuring fails with another one. clang-tidy
  rejected GCC's compiler flags, so it couldn't check a GCC build.
- hipObject now requires ROCm 7.14 or later, and configuring fails if
  `ROCM_VERSION` is earlier. The documentation used to list ROCm 6.x as
  the minimum.
- hipObject now requires CMake 3.25.2 or later, instead of 3.21. Earlier
  versions couldn't configure hipObject with amdclang, or with
  `HIPOBJ_FETCH_CUOBJECT_CLIENT` on.
- The oldest supported Ubuntu release is now 24.04, instead of 22.04,
  whose CMake is 3.22. The DEB packages depend on the libraries' Ubuntu
  24.04 package names, such as `libcurl4t64`, so they no longer install
  on 22.04.
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
    address space. `hipObjBufRegister()` also rejects a GPU buffer that
    runs past the end of the HIP allocation it's in, or whose allocation
    the HIP runtime can't find. `hipObjBufDeregister()` rejects a NULL
    pointer.
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
- The example programs, including the MinIO bridge's `minio-getput-rdma`,
  include `<hip/hip_runtime_api.h>` instead of `<hip/hip_runtime.h>`. They
  only call the HIP runtime API, so they don't need the kernel-language
  declarations, and a program that uses hipObject from host code only
  needs the same header.
- The `get-object` and `put-object` examples release what they set up
  with small guard classes, whose destructors call `hipObjBufDeregister()`,
  `hipObjShutdown()`, and `curl_global_cleanup()`, instead of calling them
  by hand on each way out of `main()`. The classes are C++11, and show
  one way to make sure an application releases hipObject's resources on
  every path. In `--live` mode, the examples now exit with an error if
  `curl_global_init()` fails.
- The public API can be called from multiple threads. Functions that use
  the library's state (`hipObjInit()`, `hipObjShutdown()`, buffer
  registration, transfers, `hipObjBufSync()`, and `hipObjGetRdmaToken()`,
  as well as `hipObjGetV2()` and `hipObjPutV2()`) are serialized: they
  run one at a time across the process, and a call waits for the one in
  progress to finish, so only one transfer runs at a time. They used to
  have no synchronization, and calling them from more than one thread at
  a time could corrupt the library's state or crash. The other functions
  can be called from any thread at any time. Each function's
  documentation now says which kind it is, and the new Thread Safety
  section of the API reference explains both, along with what
  applications have to do themselves.
- A `hipObjOps_t` callback that calls a serialized function now gets
  `hipObjInvalidValue`, since the callbacks run while the library holds
  its lock. Such calls used to run.
- `hipObjGetVersionString()` no longer writes to a shared buffer on every
  call, which was a data race when two threads called it at once.
- The library links CMake's `Threads::Threads`, which adds `-pthread`
  where the C library needs it (glibc before 2.34, for example). The
  static library's CMake package finds `Threads` for consumers, as it
  does `hip` and `hsa-runtime64`.
- Building the documentation (`HIPOBJ_BUILD_DOCS`) requires Breathe 4.36
  or later, up from 4.35. The documentation build installs it from
  `requirements.txt` into its own virtual environment.
- hipObject no longer loads other libraries or prints anything when the
  program starts. It used to load libibverbs and libnuma, and to print a
  message to stderr if libibverbs was missing, even in a program that
  never called `hipObjInit()`. hipObject now loads libibverbs, prints
  that message, and reads the `HIPOBJ_DMABUF` and `HIPOBJ_RDMA_DEBUG`
  environment variables when `hipObjInit()` first runs, so a program can
  set them before calling it. It doesn't load libnuma, which it never
  used. libibverbs stays loaded until the program exits. Building the
  tests no longer needs libnuma's development package (`libnuma-dev`).

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
- The queue pair `hipObjInit()` creates didn't allow remote reads, which
  the server needs to read a `hipObjPut()` buffer. hipObject used the wrong
  values for the libibverbs access flags, so the queue pair allowed remote
  and local writes instead of remote reads and writes. On a NIC that
  enforces a queue pair's access flags, `hipObjPut()` could fail.
- The V2 final-reply parser rejected most valid CRC64NVME checksums whose
  base64 text was not the all-zero value.
- The V2 final-reply parser could accept an `X-Amz-Rdma-Bytes-Transferred`
  value larger than `UINT64_MAX` after wrapping it to a smaller count.
- The MinIO bridge read a malformed `X-Amz-Rdma-Bytes-Transferred` value
  such as `12abc` as its leading number, and on the V2 path ignored a
  value it couldn't read at all, reporting 0 bytes. A V1 GET also
  accepted a count larger than the buffer. The transfer now fails
  instead.
- `HIPOBJ_STAGE_TIMEOUT_MS` accepted a leading `+` or whitespace, and a
  value too large for a `long` overflowed the staging copy's deadline.
  hipObject now uses the default 30-second timeout unless the value is a
  plain number of milliseconds, from 1 to 86400000 (24 hours).
- Building with GCC 15 warned about every line indented with spaces, and
  failed with `HIPOBJ_WERROR=ON`.
- Optimized Clang builds against glibc 2.40 or later warned about every
  call to `fprintf()`, `snprintf()`, and similar functions, and failed
  with `HIPOBJ_WERROR=ON`.
- The MinIO bridge (`HIPOBJ_MINIO_CLIENT=ON`) didn't build against
  libcurl 8.10 or later. The curlpp release it downloaded, v0.8.1,
  doesn't compile against them, so it now downloads a later curlpp
  commit that does.
- The MinIO bridge never used an installed curlpp (`libcurlpp-dev` on
  Ubuntu), and always downloaded and built its own. It now uses the
  installed one, and `minio-getput-rdma` links it as a shared library.
  An RPM package built that way requires `libcurlpp.so.1`.
- The MinIO bridge leaked the token it gets from `hipObjGetRdmaToken()`
  for each transfer when building or sending the request threw an
  exception. It now releases the token on every path.
- Building the MinIO bridge with GCC 14 or later warned that two
  returns in `integrations/minio-cpp/src/client.cpp` couldn't avoid a
  copy, and failed with `HIPOBJ_WERROR=ON`.
- `hipObjBufRegister()` kept a dmabuf file descriptor open for each GPU
  buffer it registered through dmabuf, until the buffer was deregistered.
  It now closes the descriptor once the registration is done. If
  allocating memory failed just after a successful registration, the
  buffer also stayed registered for remote access, and its descriptor
  leaked.
- If an application's `recvReply` callback reported a reply longer than
  the buffer hipObject passed it, `hipObjGet()` and `hipObjPut()` read
  past the end of the buffer. The transfer now fails with
  `hipObjS3Error` instead.
- The `get-object` and `put-object` examples read a size argument such
  as `12abc` as its leading number, and a negative one such as `-1` as
  a huge size. They now reject anything but a plain decimal number of
  bytes that fits in a `size_t`.

### Known issues

