# AGENTS.md

Guidance for AI coding agents working in the hipObject repository. hipObject is
an RDMA-accelerated S3 object client for AMD GPUs; see [README.md](README.md)
for scope and [docs/building.rst](docs/building.rst) for full CMake options.

## Project direction

hipObject will move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and be built by
[TheRock](https://github.com/ROCm/TheRock), ROCm's build system. Make changes
that will survive that move:

- **Ask before changing compile or link options, dependency handling, or any
  other non-trivial CMake.** A superproject has requirements that a
  standalone build doesn't show. For example, test programs are linked with
  `-pthread` (`hipobj_add_test_executable()`) because TheRock's sanitizer
  builds need it, even where it looks redundant. Never remove it, and keep
  `Threads::Threads` on whatever uses threads.
- **Don't assume hipObject is the top-level CMake project.** New CMake code
  uses `PROJECT_SOURCE_DIR`/`PROJECT_BINARY_DIR` or
  `CMAKE_CURRENT_SOURCE_DIR`/`CMAKE_CURRENT_BINARY_DIR`, not
  `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR`. Prefix new cache variables with
  `HIPOBJ_` and never force one on a parent project. Make targets with the
  `hipobj_add_*()` helpers, which give them the standard, warnings,
  hardening, and clang-tidy, and set flags per target, as
  `cmake/HIPOBJCompilerOptions.cmake` does, not globally.
- **Some known issues are left for the move**, such as `off_t` in the public
  header (see [Portability](#portability)).

The same team writes two sister projects. **hipFile** (GPU-direct file I/O,
in rocm-systems under
[`projects/hipfile`](https://github.com/ROCm/rocm-systems/tree/develop/projects/hipfile))
already builds with TheRock, so it's the reference for how hipObject should
look after the move. **rocm-ernic**
([ROCm/rocm-ernic](https://github.com/ROCm/rocm-ernic)) is the emulated NIC
that hipObject's hardware-test CI runs against. Borrow ideas from both
freely. If you find something one of them should adopt, or a bug in one, ask
whether to create a to-do file for it; don't create one or edit the other
project without asking.

## Toolchain and paths

- **ROCm**: If `ROCM_PATH` isn't set as a CMake or environment variable, it
  defaults to `/opt/rocm/core` when that holds a ROCm installation (the ROCm
  7.11+ layout) and to `/opt/rocm` otherwise; `ROCM_VERSION` changes this.
  To run binaries you build, put `/opt/rocm/bin` on `PATH` and
  `/opt/rocm/lib` in `LD_LIBRARY_PATH`. ROCm's packages leave `libamdhip64`
  out of the loader cache, so test binaries won't start without it.
- **Compiler**: hipObject is host-only C/C++ (no GPU kernels), so any C++20
  compiler works, but check changes with both GCC and Clang. The HIP headers
  and library come from the `hip::host` CMake target.

## Configure and build

From the repository root:

```bash
cmake -B build \
  -DCMAKE_PREFIX_PATH=/opt/rocm \
  -DBUILD_TESTING=ON \
  -DHIPOBJ_BNXT=ON \
  -DHIPOBJ_IONIC=ON
cmake --build build -j"$(nproc)"
```

- Both RDMA backends, `HIPOBJ_BNXT` and `HIPOBJ_IONIC`, are on by default.
- CI ([hipobject-build.yml](.github/workflows/hipobject-build.yml)) mostly
  builds with ROCm 10.1.0's amdclang on Ubuntu 26.04, which is how TheRock
  will build hipObject, with `HIPOBJ_WERROR` on. It also covers GCC 13 and
  15, upstream Clang 18 and 21, CMake 3.28, C++23 and C++26, a static build,
  and a build with both backends off; the workflow has the details.
- The MinIO C++ bridge is off by default. Turn it on with
  `-DHIPOBJ_MINIO_CLIENT=ON` (it needs the system packages listed in
  [docs/building.rst](docs/building.rst); see
  [integrations/minio-cpp/TESTING.md](integrations/minio-cpp/TESTING.md))
  when you check a change to `integrations/` or to anything it uses.

## Tests

- Run `ctest --test-dir build --output-on-failure`. The suite needs no GPU,
  RDMA NIC, or S3 server. `hipobj-package-deb` and `hipobj-package-rpm` skip
  without `dpkg`, or `rpmbuild` and `rpm`. The tests that load the system's
  libibverbs skip without it, unless `HIPOBJ_TEST_REQUIRE_LIBIBVERBS=1` (as
  in CI) makes them fail.
- Tests of the public API initialize the library with a `FakePlatform`
  ([`test/unit/fake-device.h`](test/unit/fake-device.h)), whose fakes of the
  HIP runtime, libibverbs, and the NIC topology record the calls and can make
  them fail.
- The GPU-direct and RDMA data paths are tested on emulated hardware
  (rocm-ernic's NIC and rocjitsu's GPU, under QEMU) by the
  `hipobject-hardware-test-*.yml` workflows, and in a lab with real GPUs, RDMA
  NICs, and S3 servers (see [README.md](README.md) and
  [docs/interop.rst](docs/interop.rst)).
- **The library (`src/`) must keep at least 90% line coverage from
  `ctest`**, and CI fails below that. Test new and changed code, including
  its error paths. To measure it, configure with Clang and
  `-DHIPOBJ_USE_CODE_COVERAGE=ON`, run `ctest`, and build the
  `hipobj-coverage` target, which writes `coverage/report.txt` and
  `coverage/lines.txt` in the build directory.

## Public API

- **Don't change the API in [`include/hipobj.h`](include/hipobj.h) without
  explicit permission.** The API is everything a consumer compiles or links
  against: functions and their signatures, types and struct layouts,
  enumerators and their values, and macros. Don't rename, move, reorder, or
  restyle declarations as a side effect of other work. If a change seems to
  need an API change, ask first.
- **The header's documentation comments can change without asking**, and
  should whenever a change makes them wrong or incomplete, such as a new
  requirement on callers or a new error. Check that the documentation still
  builds (see [Spelling and documentation](#spelling-and-documentation)).
- **API changes, and documentation changes that change what callers can rely
  on or have to do, need a [changelog](#changelog) entry.** Typo fixes and
  rewording don't.
- **The version is set only by the `HIPOBJ_VERSION_MAJOR`, `_MINOR`, and
  `_PATCH` macros in the header.** `CMakeLists.txt` and `docs/conf.py` read
  them, so keep each a plain `#define` of a number.
- **The header is C, for C11 and C++11 consumers.** Keep it free of
  C++-isms and of anything newer than C11 or C++11. The build compiles it
  both ways with `-Wall -Wextra -Werror -pedantic`
  ([`test/header`](test/header)).
- **The examples in [`examples/`](examples/) are written to C++11.** The
  build compiles them as C++20 with the full checks, except
  `-Wunsafe-buffer-usage`. The `hipobj-install` test builds their installed
  sources as C++11, with
  [`examples/CMakeLists.install.cmake`](examples/CMakeLists.install.cmake),
  which catches newer constructs. Add a new example's files to that file,
  [`cmake/HIPOBJInstall.cmake`](cmake/HIPOBJInstall.cmake), and
  [`test/install/run-package-test.cmake`](test/install/run-package-test.cmake).

## Thread safety

The header's `threads` group documents what callers can rely on; read it
before you add or change a public function. There's no API lock: every public
function can run at the same time as any other, on any thread or from a
callback.

- **Locks guard lookups, not calls.** Everything that changes after
  `hipObjInit()` has its own lock: the current instance (`Current` in
  [`src/state.cpp`](src/state.cpp)), the registered buffers (`BufferMap`,
  [`src/buffer.h`](src/buffer.h)), the connections (`ConnectionPool`,
  [`src/rdma/connection-pool.h`](src/rdma/connection-pool.h)), and the
  staging copies that timed out (`AbandonedCopies`,
  [`src/abandoned-copies.h`](src/abandoned-copies.h)). Hold a lock only while
  finding or changing what it guards. Anything else shared, such as a
  `static` buffer, needs a lock too, or has to be written once before any
  thread can read it.
- **Lookups copy a `std::shared_ptr` and release the lock.**
  `currentInstance()` and `BufferMap::find()` return a copy, which keeps the
  object alive even if another thread shuts the library down;
  `deregisterBuffer()` refuses to remove a buffer that anything else holds. A
  transfer holds its connection through a `ConnectionPool::Lease`. Don't keep
  a raw pointer or reference into a locked container after its lock is
  released.
- **Hold no lock across a callback, a libibverbs call, or a HIP call.** The
  exceptions are `initWith()` and `shutdownInstance()`, which hold
  `Current::initMutex` (nothing else takes it), and `AbandonedCopies`, which
  holds its lock across `eventQuery()`, which doesn't wait, and never with
  another lock.
- **Take locks in this order**, and only those a step needs:
  `Current::initMutex`, then `Current::mutex`, `BufferMap`'s, and
  `ConnectionPool`'s.
- **Declare what each lock guards, for Clang's thread safety analysis.** Use
  the wrappers in
  [`src/common/thread-safety.h`](src/common/thread-safety.h) (`Mutex`,
  `SharedMutex`, `ScopedLock`, `SharedLock`, and `ConditionVariable`), not
  the standard library's locks. Mark guarded data with
  `HIPOBJ_GUARDED_BY()`, a function that needs a lock held with
  `HIPOBJ_REQUIRES()` or `HIPOBJ_REQUIRES_SHARED()`, and one that takes its
  class's lock with `HIPOBJ_REQUIRES(!mutex_)`. Only Clang checks this, and
  the analysis misses what the header lists, such as constructors and
  destructors, so it doesn't replace ThreadSanitizer.
- **Don't wait for something the calling thread may hold.** A callback runs
  on its transfer's thread, so `ConnectionPool::acquire()` returns `Busy` to
  a thread that already holds a lease. Any new wait needs the same care.
- **Don't call a public function from inside the library.** It could find a
  different instance than the call it's part of. Pass on the `Instance` the
  call found.
- **Test concurrency changes with ThreadSanitizer**, and add each new public
  function to
  [`test/unit/test-thread-safety.cpp`](test/unit/test-thread-safety.cpp),
  including the test whose callback calls every function.

## C++ code

- **The library, tests, and tools are C++20**, without GNU extensions
  (`HIPOBJ_CXX_STANDARD` also accepts 23, and 26 with CMake 3.30 or later).
- **Write modern C++.** Only the public header has to look like C. Don't
  copy the C-style code that remains.
  - Own resources with RAII: `std::unique_ptr` (`std::shared_ptr` only when
    ownership really is shared), not raw owning pointers, `new`/`delete`, or
    `malloc`/`free`. Wrap handles from C libraries (libibverbs, libcurl,
    `dlopen`) in a `std::unique_ptr` with a custom deleter or a small RAII
    class. Use raw pointers only for non-owning access.
  - Prefer `std::string` and `std::string_view`, `std::array`,
    `std::vector`, and `std::span`, `std::optional`, `enum class`,
    `constexpr`, and C++ casts to `char` buffers, C arrays,
    pointer-and-length pairs, sentinel values, macros, and C casts. Use the
    annotated lock wrappers (see [Thread safety](#thread-safety)), not
    pthreads.
  - Include `<cstdlib>`, `<cstring>`, `<cstdint>`, and so on, not
    `<stdlib.h>` and the like, and use the `std::` names. Only headers that C
    code includes, such as the public header, use the C names.
  - Don't let exceptions cross the C API. The public functions in
    `src/hipobj.cpp` catch everything and return an error code.
  - Make the `hipObjError_t` a public function returns with
    `hipObj::makeError()` ([`src/hipobj-private.h`](src/hipobj-private.h)):
    `return makeError(hipObjInvalidValue);`, or
    `makeError(hipObjRdmaError, static_cast<int>(hipErr))` to pass on a HIP
    error. clang-tidy rejects `{hipObjInvalidValue, 0}`. Code outside the
    library, such as the MinIO bridge, uses designated initializers
    (`{.opError = hipObjSuccess, .hipError = 0}`).
- **Don't add global accessors.** The library's one global is the current
  `Instance` (`currentInstance()`, [`src/state.h`](src/state.h)), which the
  C API needs. Everything below it gets what it uses from its constructor or
  parameters. The outside world comes in through the `Platform` given to
  `initWith()`, as the `HipRuntime`, `Verbs`, and `NicTopology` interfaces:
  `systemPlatform()` in production, and a `FakePlatform` in tests.
  - Pass the narrowest dependency (`const Verbs &`, not the `Platform`). An
    object that uses it later holds a reference or pointer to it, as
    `BufferMap` and `RdmaDevice` do; the `Instance` keeps the platform alive.
  - Don't add a function that returns a process-wide object, or a hook that
    swaps one for a test. Dependencies are set when an object is built and
    never replaced, so tests run the same code as production.
  - Interfaces are only for the outside world. Don't add one to fake an
    internal class.
- **Don't do arithmetic on raw pointers.** Clang's `-Wunsafe-buffer-usage`
  flags pointer arithmetic, indexing through a raw pointer, the C memory and
  string functions (`memcpy()`, `strlen()`, and the rest), and a
  `std::span` or `std::string_view` built from a pointer and a separate
  length. Use containers, views, and the standard algorithms
  (`std::ranges::copy()`, `std::ranges::fill()`). Turn memory that comes in
  through a C interface (the public API, libibverbs, `argv`) into a view
  once, where it comes in, with `spanFromPointer()`, `cStringView()`, or
  `argSpan()` from [`shared/hipobj-span.h`](shared/hipobj-span.h), which
  holds the only suppressions of the warning.
- **Don't write `reinterpret_cast`** (clang-tidy reports it). Use
  `pointerToAddress()` and `addressToPointer()` from
  [`shared/hipobj-address.h`](shared/hipobj-address.h), and `asSockaddr()`
  from [`shared/hipobj-sockaddr.h`](shared/hipobj-sockaddr.h). Those headers
  hold the only suppressions; add any other conversion the code needs there.
- **Don't convert numbers with `ato*()`, `strto*()`, or `sscanf()`.** They
  miss errors, accept whitespace and signs, or have undefined behavior when
  a value doesn't fit. Use `hipObj::parseNumber()` from
  [`shared/hipobj-parse.h`](shared/hipobj-parse.h), which rejects empty
  text, trailing characters, and out-of-range values, or `std::from_chars`,
  checking both its error code and that it consumed all the text.
- **Don't print with `printf()` or its relatives.** clang-tidy reports every
  C-style variadic call (`cppcoreguidelines-pro-type-vararg`). Use
  `hipObj::printTo()` from [`shared/hipobj-print.h`](shared/hipobj-print.h),
  as in `printTo(stderr, "hipObj: {} bytes\n", size)`, which checks its
  format string at compile time and never throws, and `std::format()` or
  `std::format_to_n()`. For other variadic functions, use one that isn't
  variadic where there is one (`fstat()`, not `fcntl()`, to check that a
  file descriptor is open).
- **The C++11 examples are the exception** to the last two rules. They use
  `std::strtoull()`, checking `errno`, the end pointer, and the first
  character, and print with `printf()` and `fprintf()`.

## Portability

hipObject only builds on Linux, but may be built for Windows someday.

- **Don't use `long` or `unsigned long` for sizes, offsets, or anything that
  has to be 64 bits**; it's 32 bits on Windows. Use `size_t`, `ptrdiff_t`,
  and the `<cstdint>` fixed-width types, especially for values on the wire.
  Watch for `%ld` and `%lu` in the examples' `printf()` calls (use `PRIu64`
  and the like, or `%zu`), and for `L` and `UL` literal suffixes.
- **`off_t` in the public header is a known exception**, to be fixed in
  rocm-systems. Don't change it, and don't spread it into internal code: use
  a fixed-width type there, and convert, with a range check, at the API
  boundary.
- **Keep Linux-specific code behind the `Platform` interfaces**
  ([`src/state.h`](src/state.h)). Their production implementations
  (`SystemHip`; `LoadedVerbs`, which loads libibverbs with `dlopen()`; and
  `SysfsTopology`) are the only library code that calls the HIP runtime,
  libibverbs, or sysfs. Give anything new from the outside world, such as a
  random source, its own interface in `Platform`.

## Security

**Security is critical.** hipObject hands remote peers direct access to GPU and
host memory and parses whatever the network sends back, so security takes
priority over speed and convenience.

- **Fully initialize anything that touches the wire**: tokens, HTTP headers
  and bodies, and every verbs structure (work requests, scatter/gather
  entries, QP attributes). Use `T x{}`, or for C structs
  `auto x = hipObj::zeroedObject<T>()` from
  [`shared/hipobj-span.h`](shared/hipobj-span.h), which zeroes the padding
  too; zero an existing one with `hipObj::zeroObject()`. Serialize field by
  field; never copy a struct's raw bytes onto the wire.
- **Treat everything from the network as untrusted**: S3 responses,
  `x-amz-rdma-reply` values, peer tokens, and completions. Check each
  field's exact length or width, characters, and range, and reject
  malformed input with an error rather than truncating, clamping, or
  guessing. Past parsers have accepted a sign, a fourth digit in an HTTP
  status, junk after the last field, an empty header, a value too wide for
  a 24-bit field, a reserved zero, and a peer token after a non-200 status,
  so test the malformed forms. Never use a peer-supplied remote address,
  rkey, length, or offset without checking it against what was agreed.
- **Make integer arithmetic safe.** Check for wrap-around before it happens
  (`__builtin_add_overflow`, `__builtin_mul_overflow`, or comparing against
  the limit first). Check ranges before narrowing, such as a `uint64_t` into
  a 32-bit verbs length. Use `std::in_range` and the `std::cmp_*` functions
  for signed/unsigned comparisons (`off_t` is signed). Avoid signed
  overflow, shifts by a negative amount or by the type's width or more, and
  casts of out-of-range values. No sanitizer reports unsigned wrap-around.
- **Bound every buffer.** No `strcpy`, `sprintf`, or unbounded `strcat`.
  Where the C API fills a caller's buffer (such as
  `hipObjTokenClientNic()`), respect its length and always NUL-terminate.
- **Grant the least memory access needed** when registering memory and
  setting QP access flags.
- **Keep secrets secret.** Don't log, print, or put in error messages the S3
  credentials or RDMA tokens, rkeys, or remote addresses, even in debug
  output. Anything that must be unpredictable, such as a session ID, comes
  from `getrandom()` behind an internal interface, never from `rand()` or a
  `<random>` engine.
- **Keep the hardening flags**: `_FORTIFY_SOURCE`, the stack protector,
  `-fstack-clash-protection`, and the linker hardening in
  `hipobj_set_linker_flags()`: PIE, full RELRO, a non-executable stack, and
  `--as-needed`.
- **Fix security scan findings** from CodeQL and the PR security scan
  (bandit, gitleaks, trivy, and zizmor); don't dismiss them.
- **Make fake secrets in tests look fake.** Gitleaks flags constants that
  look like real keys, such as a hex string in a variable named for a token,
  even in tests. Use an obvious placeholder, such as `"rdma-test-token"`,
  keeping any length or character set the code needs, but not a plain hex or
  base64 string. Computed values are fine. To check:

  ```bash
  d=$(mktemp -d)
  git ls-files -z | xargs -0 tar -cf - | tar -xf - -C "$d"
  gitleaks dir "$d" --config .github/scan_tools_configs/gitleaks.toml
  rm -rf "$d"
  ```

## Data path performance

The hot path is everything that runs once per transfer: `hipObjGet()` and
`hipObjPut()`, minting and parsing RDMA tokens and replies, `hipObjBufSync()`,
and posting and polling RDMA work. `hipObjInit()`, buffer registration, and
shutdown are setup.

- **Do expensive work once, at setup**: opening devices, choosing the NIC,
  creating queue pairs, registering memory, exporting dmabufs, and reading
  sysfs.
- **Keep the hot path lean.** Avoid heap allocations, copying the payload
  (only the host-staged fallback does), string formatting and logging on
  success, unneeded system calls, and HIP calls that synchronize. Don't add
  locks without a reason, and hold them briefly. Watch for constructs that
  cost something, such as copying a `std::shared_ptr`, a `std::function`
  that allocates, or a `std::string` built per transfer.
- **Don't give up correctness or safety for speed.** If a change has to add
  work to the hot path, say so and explain why.

## Linters, warnings, and sanitizers (mandatory)

**Every change must pass every check below that covers the files it touches.
A change with an outstanding finding isn't finished.** CI doesn't run all of
these checks, and some of its jobs run only when certain paths change, so a
green PR doesn't prove a change is clean. Run them yourself, from the
repository root.

Fix the code rather than suppressing a finding (`// NOLINT`,
`# shellcheck disable=`, `# pylint: disable=`, `-Wno-…`). Suppress one only
when it's unavoidable, for example because of how Google Test works, and
comment why.

### C and C++

- **clang-format 18**: reformat with `util/format-source.sh 18`, or check:

  ```bash
  git ls-files -z '*.c' '*.h' '*.cpp' '*.hpp' \
    | xargs -0 clang-format-18 --dry-run -Werror
  ```

- **No compiler warnings with GCC or Clang.** Build with each, in separate
  build directories, and configure with `-DHIPOBJ_WERROR=ON` or read the
  build output.
- **Suppress a warning only when it's unavoidable, and only with the macros
  in [`shared/hipobj-warnings.h`](shared/hipobj-warnings.h)**, never raw
  `#pragma`s, `-Wno-…` flags, or changes to the warning lists. Add an
  `HIPOBJ_WARN_<NAME>_OFF`/`_ON` pair there (empty for compilers without
  the warning), add both to `StatementMacros` in
  [`.clang-format`](.clang-format), wrap the smallest region you can, and
  comment why. Third-party code fetched with `FetchContent` is exempt.
- **clang-tidy**: configure with Clang and `-DHIPOBJ_USE_CLANG_TIDY=ON`, and
  build; it must report nothing. It needs clang-tidy 19 or later. Prefer an
  upstream one to ROCm's, which lacks the `clang-analyzer-*` checks; set
  `HIPOBJ_CLANG_TIDY_EXE` to choose. The `.clang-tidy` files in `examples/`,
  `test/unit/`, and `integrations/minio-cpp/test/` relax a few checks; other
  code, the test server included, names its constants.
- **include-what-you-use**: configure a separate build directory, only for
  IWYU, with Clang and `-DHIPOBJ_USE_IWYU=ON`, and build. It doesn't have to
  report nothing. Make its correct suggestions for the files you touch, and
  ignore these wrong ones:
  - `<compare>` where `std::chrono` time points are compared. `<chrono>`
    already includes it.
  - Forward declarations of verbs types (`struct ibv_pd;`) in place of
    `ibv-core.h`. Inside `namespace hipObj`, they would declare different
    types.
  - Removing `<cstdint>` from `src/hipobj.cpp`.
  - Adding `<memory>` "for allocator" where a file only uses containers.
  - Removing a forward declaration, with no text, at a lambda's line.
  - Naming a library's internal header rather than its public one. Fix these
    with a mapping in a `cmake/iwyu-*.imp` file, as
    [`cmake/iwyu-hip.imp`](cmake/iwyu-hip.imp) does.
- **Sanitizers**: with Clang, build one directory with
  `-DHIPOBJ_USE_SANITIZERS=ON` (AddressSanitizer, LeakSanitizer,
  UndefinedBehaviorSanitizer, and more) and one with
  `-DHIPOBJ_USE_THREAD_SANITIZER=ON`, and run `ctest` in each. Every report
  fails its test. Use GCC too when a change could behave differently with it.
  The first configure fetches GoogleTest, so it needs network access. Add
  sanitizer flags only in
  [`HIPOBJSanitizers.cmake`](cmake/HIPOBJSanitizers.cmake).
- **Valgrind**: run memcheck in the GCC build, without the sanitizers. It
  finds reads of uninitialized memory that the sanitizers miss:

  ```bash
  ctest --test-dir build -T memcheck --output-on-failure \
    --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
    --overwrite "MemoryCheckCommandOptions=--leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1" \
    -E '^hipobj-(exports|subproject|install|package-deb|package-rpm)$'
  ```

  The reports go to `build/Testing/Temporary/MemoryChecker.*.log`, not to
  ctest's output. To see memory that's still reachable at exit, add
  `--show-leak-kinds=all --errors-for-leak-kinds=all` to the options, and
  `--overwrite "MemoryCheckSuppressionFile=$PWD/test/valgrind.supp"` (the
  path has to be absolute). Don't use an optimized Clang build: its
  `std::format()` code uses x87 instructions that valgrind emulates wrongly.
  A Clang Debug build works.
- **Lifetime safety**, when a change creates, moves, or stores a pointer,
  reference, iterator, or view into memory that something else owns, or
  destroys, moves, or resizes an object that other code may point into.
  Build once with Clang 23's `-Wlifetime-safety`:

  ```bash
  cmake -B build-lifetime -DCMAKE_C_COMPILER=clang-23 \
    -DCMAKE_CXX_COMPILER=clang++-23 -DCMAKE_PREFIX_PATH=/opt/rocm \
    -DBUILD_TESTING=ON -DHIPOBJ_BNXT=ON -DHIPOBJ_IONIC=ON \
    -DCMAKE_CXX_FLAGS=-Wlifetime-safety
  cmake --build build-lifetime -j"$(nproc)"
  ```

  Fix the real reports for the files you touched. Leave the code alone for
  false positives, which mostly come from `-Wlifetime-safety-invalidation`
  (such as a `std::string &` parameter appended to twice), and don't
  suppress them.

### Shell

Shell scripts are bash: start them with `#!/usr/bin/env bash`, and use
`shell: bash` (or the workflows' `bash -e {0}` default) for `run:` steps.

- **shellcheck**: `git ls-files -z '*.sh' | xargs -0 shellcheck`
- **actionlint** for the `run:` blocks in `.github/workflows/*.yml`. It runs
  shellcheck on each one, so shellcheck must be on `PATH`. The `-ignore`
  hides a spurious report about the `uses: $/...` syntax:

  ```bash
  actionlint -ignore 'specifying action "\$/'
  ```

  actionlint doesn't read composite actions, so copy each `run:` step in
  `.github/actions/*/action.yml` into a file with a bash shebang and run
  shellcheck on it. Neither tool sees the bodies of heredocs, such as the
  scripts the hardware-test workflows run in their guest VMs, so check those
  by hand.

### CMake

- **cmakelint** 1.4.3:

  ```bash
  git ls-files -z 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' \
    | xargs -0 cmakelint
  ```

### Python

Python code must run on **Python 3.10**, so nothing newer, such as
`tomllib`, `except*`, or `typing.Self`. Python embedded in other files, such
as workflow heredocs, has to be black-formatted by hand.

```bash
git ls-files -z '*.py' | xargs -0 black --check --target-version py310
git ls-files -z '*.py' | xargs -0 pylint --py-version=3.10
git ls-files -z '*.py' | xargs -0 vermin -t=3.10- --violations --no-tips
```

If black's output differs from CI's, use the versions
[hipobject-pylint.yml](.github/workflows/hipobject-pylint.yml) pins.

### Spelling and documentation

- **codespell**, over the whole tree: `codespell`
- **doc8**: `doc8 docs/ --max-line-length 80`
- **The documentation must build** (this doesn't need ROCm):

  ```bash
  cmake -B build-docs -DHIPOBJ_DOCS_ONLY=ON -DHIPOBJ_BUILD_DOCS=ON
  cmake --build build-docs --target hipobj-sphinx-html
  ```

- **It must build without CMake too**, since Read the Docs will run doxygen
  in `docs/doxygen` and Sphinx on `docs/conf.py` directly. Keep CMake's
  `@VARIABLE@` substitutions out of both.

### Ansible

- **ansible-lint** and a playbook syntax check, run from `ansible/`; see
  [ansible/README.md](ansible/README.md) and
  [hipobject-ansible.yml](.github/workflows/hipobject-ansible.yml) for the
  commands.

## Changelog

**Every user-facing change needs an entry in [CHANGELOG.md](CHANGELOG.md), in
the same change.** That's anything someone building, installing, or calling
hipObject could notice: the public API and what its documentation promises,
library behavior, CMake options and variables, installed files and packages,
dependencies, supported platforms, and the examples. Small changes count,
such as a new compiler warning, a change to how CMake finds a tool, or a
reworded error message. CI-only, test-only, and internal refactoring changes
don't need one.

Add the entry to the topmost (unreleased) version, under `Added`, `Changed`,
`Removed`, or `Known issues`, or a new heading such as `Resolved issues` if
none fits. Say what changed, and what users have to do differently, if
anything.

## Review your change before you finish

When you've modified the repository, review the whole change as a reviewer
would. Read all of `git diff`, and every new file `git status` lists, from
start to finish. Every PR gets a Copilot review, and these are what it asks
for most often:

- **A missing or wrong changelog entry.** An entry must not claim more, or
  less, than the diff does.
- **A new file left out of the change.** Name each new file when you hand
  the change back, so it gets committed.
- **An incomplete fix.** Search for every other place with the same pattern,
  and fix them too or say why not.
- **A test that passes without the fix.** Revert the fix in a scratch copy
  and check. Have `FakePlatform` fail the call or return the edge case, and
  test new error paths too.
- **Cleanup that's wrong on some path.** Follow each failure branch, and ask
  what happens if an allocation or a container insertion throws. Members are
  destroyed after the destructor's body, in reverse declaration order, so
  declaration order is teardown order: a queue pair goes before the memory
  region its work uses, and a memory region before the memory it registers.
- **A script that turns a failure into success**, such as a retry loop whose
  last `sleep` succeeds, a pipeline without `pipefail`, or an exit code that
  means both "absent" and "lookup failed".
- **Words that don't match the code.** Comments, documentation, changelog
  entries, and the PR description must claim only what the diff does. For
  example, don't state a guarantee that holds only on success as if it held
  on every return.

Fix what you find, then run the
[checks](#linters-warnings-and-sanitizers-mandatory) again.

## Lab provisioning

For GPU hosts, ROCm, RDMA, and MinIO AIStor installs, use
[ansible/README.md](ansible/README.md).
