# Changelog for hipObject

## (Unreleased) hipObject 0.1.0

### Added

- `hipobj-test-install`, a program in the runtime package, installed in
  `bin`. It will check that hipObject is installed and works, as far as
  the system's hardware allows, without an S3 server. For now it only
  reports the hipObject version, with `--version`. The new
  `HIPOBJ_INSTALL_TOOLS` CMake option (default `ON`) builds and installs
  it. A static build installs it but doesn't package it.
- hipObject is installed and packaged with rocm-cmake. `cpack` builds
  `hipobject<mm>` (runtime) and `hipobject-dev<mm>` (development) DEB
  packages or `hipobject<mm>` and `hipobject-devel<mm>` RPM packages,
  or a single `hipobject-static-dev<mm>` DEB or
  `hipobject-static-devel<mm>` RPM package for a static build, where
  `<mm>` is the ROCm major and minor version, as in ROCm's own package
  names (`hipobject10.1`). Packages for different ROCm versions can be
  installed together when each installs into its ROCm version's
  directory, `/opt/rocm/core-<mm>`. The development package requires
  the runtime package from the same build, with the same version and
  release. Each package replaces the package of the same kind that
  earlier nightly packages had, without the ROCm version in its name.
  rocm-cmake is downloaded if ROCm doesn't provide it. The package that
  holds the library recommends libibverbs, which RDMA needs:
  `libibverbs1` (DEB) or `libibverbs.so.1()(64bit)` (RPM). An RPM
  package built with a CMake before 4.1, which can't add a Recommends,
  doesn't.
- Nightly DEB packages of the `develop` branch, for Ubuntu 24.04 and
  26.04 with ROCm 10.1.0 and 7.14.1, are published as the `nightly`
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
- The `hipObjBusy` error code, after `hipObjInternalError`, so no other
  code's value changes. A call returns it when the buffer it's given is
  in use: a staging copy of the buffer that timed out is still running,
  or, for `hipObjBufDeregister()`, another call is using the buffer. A
  transfer started from a `hipObjOps_t` callback also returns it when
  every RDMA connection is in use. It isn't the `hipObjBusy` that was
  removed with the V2 interface, and cuObject has no equivalent.
- The `HIPOBJ_MAX_TRANSFERS` environment variable sets how many RDMA
  connections (a queue pair and its completion queue each)
  `hipObjInit()` creates for transfers. Each `hipObjGet()` and
  `hipObjPut()` uses one of them, rather than every transfer sharing one
  queue pair, so it's how many transfers can run at once; a transfer
  that starts while they're all in use waits for one. The value is a
  whole number from 1 to 1024, and a value that isn't one is ignored,
  leaving the default of 16. Like the other
  environment variables, it's read each time `hipObjInit()` runs, which
  fails with `hipObjRdmaError` if it can't create them all.
  `hipObjGetRdmaToken()` still advertises a queue pair of its own, which
  no transfer uses.

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
- The `get-object` and `put-object` examples are installed as source,
  not as programs. Their sources, a `CMakeLists.txt` that builds them
  against the installed hipObject as C++11, and a `README.md` on building
  and running them are installed in `share/doc/hipobject<mm>/examples`,
  in the development package (`hipobject-dev<mm>`, `hipobject-devel<mm>`,
  or a static build's one package). The runtime package no longer has
  `bin/hipobj-examples/get-object` and `put-object`, and no longer
  depends on libcurl unless it has the MinIO bridge's
  `minio-getput-rdma`. To run the examples, build them from the
  installed sources. `HIPOBJ_INSTALL_EXAMPLES` still turns the examples
  on and off, and still builds them in the build tree.
- The public API can be called from multiple threads, and transfers run
  concurrently. Every function can be called from any thread at any
  time, including from a `hipObjOps_t` callback. The functions used to
  have no synchronization, and calling them from more than one thread at
  a time could corrupt the library's state or crash. The new Thread
  Safety section of the API reference describes what applications can
  rely on, and what they have to do themselves, such as for a transfer
  they drive with `hipObjGetRdmaToken()`. In particular:
  - `hipObjGet()` and `hipObjPut()` each use an RDMA connection of their
    own (see `HIPOBJ_MAX_TRANSFERS`, above), and wait for one when they're
    all in use. A transfer that a callback starts returns `hipObjBusy`
    instead of waiting, since the connection it would wait for could be
    its caller's.
  - `hipObjBufDeregister()` returns `hipObjBusy`, and leaves the buffer
    registered, while another call is using it, including the transfer
    whose callback calls it. It used to deregister the buffer.
  - `hipObjShutdown()` returns without waiting for the calls in progress,
    which finish normally. hipObject releases the RDMA resources and
    buffer registrations when the last of them returns, and
    `hipObjInit()` can initialize it again before then.
- `hipObjGetVersionString()` no longer writes to a shared buffer on every
  call, which was a data race when two threads called it at once.
- The library links CMake's `Threads::Threads`, which adds `-pthread`
  where the C library needs it (glibc before 2.34, for example). The
  static library's CMake package finds `Threads` for consumers, as it
  does `hip` and `hsa-runtime64`.
- The library, the examples, and the tests are linked with the OpenSSF
  hardening options whenever the linker supports them: the programs as
  position-independent executables, and everything with full RELRO
  (`-z relro -z now`), a non-executable stack (`-z noexecstack`), and
  `--as-needed --no-copy-dt-needed-entries`. Ubuntu's GCC already did
  all of this by default, but other toolchains, such as upstream Clang,
  didn't. As a result, `libhipobj.so` and the programs no longer record a
  dependency on a shared library they don't call, such as `libm`.
- Building the documentation (`HIPOBJ_BUILD_DOCS`) requires Breathe 4.36
  or later, up from 4.35. The documentation build installs it from
  `requirements.txt` into its own virtual environment.
- The documentation build's targets are now `hipobj-sphinx-html`,
  `hipobj-doxygen`, and `hipobj-docs-venv`, rather than `sphinx-html`,
  `doxygen`, and `docs-venv`, so they can't collide with another
  project's targets when hipObject is built as part of one. Build the
  HTML documentation with `cmake --build build --target
  hipobj-sphinx-html`.
- hipObject no longer loads other libraries or prints anything when the
  program starts. It used to load libibverbs and libnuma, and to print a
  message to stderr if libibverbs was missing, even in a program that
  never called `hipObjInit()`. hipObject now loads libibverbs and prints
  that message when `hipObjInit()` first runs. It reads the
  `HIPOBJ_DMABUF` and `HIPOBJ_RDMA_DEBUG` environment variables in
  `hipObjInit()`, as described below. It doesn't load libnuma, which it
  never used. libibverbs stays loaded until the program exits. Building
  the tests no longer needs libnuma's development package
  (`libnuma-dev`).
- hipObject reads the `HIPOBJ_STAGE_TIMEOUT_MS`,
  `HIPOBJ_REQUIRE_GPU_DIRECT`, `HIPOBJ_DMABUF`, and `HIPOBJ_RDMA_DEBUG`
  environment variables each time `hipObjInit()` runs, rather than once
  per process, so a change takes effect at the next initialization. A
  program can change them after `hipObjShutdown()` and call
  `hipObjInit()` again to use the new values.
- For a buffer that's staged through host memory, `hipObjGet()`,
  `hipObjPut()`, and `hipObjBufSync()` now fail with
  `hipObjInternalError` if the HIP runtime can't create the event that
  limits how long the staging copy can take (`HIPOBJ_STAGE_TIMEOUT_MS`).
  They used to copy with `hipMemcpy()` instead, with no time limit, so a
  copy that never finished could hang the call.
- The staging copies for a buffer that's staged through host memory run
  on the calling thread's default stream (`hipStreamPerThread`), rather
  than the null stream, so copies from different threads don't wait for
  each other. As a result, they no longer wait for GPU work that the
  application queued on streams it created. Before `hipObjPut()` or
  `hipObjBufSync()` with `HIPOBJ_SYNC_TO_HOST`, make sure the GPU work
  that writes the buffer has finished, as a buffer that the NIC reads
  directly already requires. The documentation of `hipObjGet()`,
  `hipObjPut()`, and `hipObjBufSync()` now says what GPU work has to
  finish before each call. It also says that when a staging copy doesn't
  finish within `HIPOBJ_STAGE_TIMEOUT_MS`, the call returns
  `hipObjInternalError` while the copy may still be using the buffer.

### Removed

- The `OFFLOAD_ARCH` CMake cache variable and its `rocminfo`-based GPU
  architecture detection. It was never used to set the target architecture
  and is unnecessary now that nothing is compiled for the GPU.
- The experimental V2 interface: `hipObjGetV2()`, `hipObjPutV2()`, the
  types they took, the `hipObjNotSupported` and `hipObjBusy` error codes,
  and the `HIPOBJECT_V2_API` CMake option. The V2 functions were never
  implemented: they checked their arguments, returning an error such as
  `hipObjInvalidValue` for bad ones, and otherwise returned
  `hipObjNotSupported` without transferring anything. Use `hipObjGet()` and
  `hipObjPut()` instead.
- The `HIPOBJ_SUCCESS` macro from `hipobj.h`. It expanded to a compound
  literal, which isn't valid C++. Compare a `hipObjError_t`'s `opError`
  with `hipObjSuccess` instead.

### Resolved issues

- `hipObjGet()`, `hipObjPut()`, and `hipObjBufSync()` could accept an
  offset and size whose sum wrapped around, and advertise a range outside
  the registered buffer to the server or copy outside it.
- When a staging copy didn't finish within `HIPOBJ_STAGE_TIMEOUT_MS`,
  `hipObjBufDeregister()` and `hipObjShutdown()` freed the staging buffer
  while the copy could still use it, and later transfers could use the
  staging buffer at the same time as the copy. hipObject now keeps the
  staging buffer until the copy finishes. Until then, `hipObjGet()`,
  `hipObjPut()`, `hipObjBufSync()`, and `hipObjGetRdmaToken()` return
  the new `hipObjBusy` error for the buffer, and `hipObjBufDeregister()`
  returns `hipObjBusy` and leaves it registered; call it again once the
  copy has finished. If the copy never finishes, the staging
  buffer is never freed.
- The queue pair `hipObjInit()` creates didn't allow remote reads, which
  the server needs to read a `hipObjPut()` buffer. hipObject used the wrong
  values for the libibverbs access flags, so the queue pair allowed remote
  and local writes instead of remote reads and writes. On a NIC that
  enforces a queue pair's access flags, `hipObjPut()` could fail.
- The MinIO bridge read a malformed `X-Amz-Rdma-Bytes-Transferred` value
  such as `12abc` as its leading number, and a GET accepted a count
  larger than the buffer. The transfer now fails instead.
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
- On a real NIC, once a server's reply to `hipObjGet()` or `hipObjPut()`
  had named its queue pair, which connects the client's queue pair to
  it, every later transfer whose reply did the same failed with
  `hipObjRdmaError`, since a connected queue pair can't connect again.
  hipObject now resets a queue pair after each transfer that connected
  it, so the next transfer can connect it to another peer.
- The `get-object` and `put-object` examples read a size argument such
  as `12abc` as its leading number, and a negative one such as `-1` as
  a huge size. They now reject anything but a plain decimal number of
  bytes that fits in a `size_t`.
- Building the `get-object` and `put-object` examples without libcurl
  warned that `bucket` and `object` were set but never used, and failed
  with `HIPOBJ_WERROR=ON`, or with warnings as errors when building the
  installed examples.
- `hipObjInit()` failed with `hipObjRdmaError` unless libibverbs'
  development package (`libibverbs-dev` or `rdma-core-devel`) was
  installed, since hipObject loaded the unversioned `libibverbs.so` link
  that only that package installs. It now loads `libibverbs.so.1`, which
  the runtime package (`libibverbs1` or `libibverbs`) installs, from the
  loader's search path, and no longer also looks in
  `/usr/lib/x86_64-linux-gnu`, which the loader already searches on the
  systems that have it. When libibverbs doesn't load, the message
  `hipObjInit()` prints now says why, and no longer claims that RDMA is
  disabled.

### Known issues

