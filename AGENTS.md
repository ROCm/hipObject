# AGENTS.md

Guidance for AI coding agents working in the hipObject repository. hipObject is
an RDMA-accelerated S3 object client for AMD GPUs; see [README.md](README.md)
for scope and [docs/building.rst](docs/building.rst) for full CMake options.

## Project direction

hipObject will eventually move into the
[rocm-systems](https://github.com/ROCm/rocm-systems) monorepo and be built by
[TheRock](https://github.com/ROCm/TheRock), ROCm's build system. Make changes
that will survive that move:

- **Ask before changing compile or link options, dependency handling, or any
  other non-trivial CMake.** A superproject build has requirements that a
  standalone build doesn't show, so a change that works here can break there.
  For example, test programs are compiled and linked with `-pthread`
  (`hipobj_add_test_executable()`), even where it looks redundant, because
  TheRock's sanitizer builds need it. Never remove it.
- **Don't assume hipObject is the top-level CMake project.** In new CMake code,
  use `PROJECT_SOURCE_DIR`/`PROJECT_BINARY_DIR` or
  `CMAKE_CURRENT_SOURCE_DIR`/`CMAKE_CURRENT_BINARY_DIR` rather than
  `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR` (existing code still uses the latter in
  many places). Prefix new cache variables with `HIPOBJ_` and new targets with
  `hipobj`, and set flags per target, as `cmake/HIPOBJCompilerOptions.cmake`
  does, rather than globally.
- **Some known issues are deliberately left for the move**, such as `off_t` in
  the public header (see [Portability](#portability)).

### Sister projects

The same team writes two sister projects:

- **hipFile**, GPU-direct file I/O, in rocm-systems under
  [`projects/hipfile`](https://github.com/ROCm/rocm-systems/tree/develop/projects/hipfile).
  It already lives in rocm-systems and builds with TheRock, so it's the
  reference for how hipObject should look after the move.
- **rocm-ernic**, in its own repository for now
  ([ROCm/rocm-ernic](https://github.com/ROCm/rocm-ernic)). hipObject's
  emulated-hardware CI lanes run against it.

Borrow ideas from them freely, as hipObject already has: its `.clang-format`
and `shared/hipobj-warnings.h` came from hipFile. Ideas can flow the other way
too. If you find something in hipObject that one of them should adopt, or a bug
or improvement in them, ask whether to create a to-do file for it; don't create
one or edit the other project without asking.

## Toolchain and paths

- **ROCm**: If `ROCM_PATH` isn't set as a CMake or environment variable, it
  defaults to `/opt/rocm/core` when that holds a ROCm installation (the ROCm
  7.11+ layout) and to `/opt/rocm` otherwise; setting `ROCM_VERSION` changes
  this (see [docs/building.rst](docs/building.rst)). Put `/opt/rocm/bin` on
  `PATH` and include `/opt/rocm/lib` in `LD_LIBRARY_PATH` when running binaries
  you build. The `rocm/dev-ubuntu-24.04` full image that CI uses leaves
  `libamdhip64` out of the loader cache, so test binaries won't start there
  without it.
- **Compiler**: hipObject is host-only C/C++ (no GPU kernels), so any C++20
  compiler works, though changes have to be checked with both GCC and Clang
  (see [C and C++](#c-and-c)). The HIP runtime headers and library come from
  the `hip::host` CMake target, not from compiling sources as HIP.

## Configure and build (default agent flow)

From the repository root, match CI:

```bash
cmake -B build \
  -DCMAKE_PREFIX_PATH=/opt/rocm \
  -DBUILD_TESTING=ON \
  -DHIPOBJ_BNXT=ON \
  -DHIPOBJ_IONIC=ON
cmake --build build -j"$(nproc)"
```

Notes:

- The build steps in [docs/building.rst](docs/building.rst) set `HIPOBJ_IONIC`
  to **OFF** for local lab workflows; CI uses **ON** to compile both RDMA
  backends—keep that difference in mind when debugging.
- CI ([hipobject-build.yml](.github/workflows/hipobject-build.yml)) builds this
  configuration in the `rocm/dev-ubuntu-24.04:10.0.0-full` image, along with
  variants that add `-DHIPOBJECT_V2_API=OFF`, `-DBUILD_SHARED_LIBS=OFF`,
  `-DHIPOBJ_USE_SANITIZERS=ON` (AddressSanitizer), or
  `-DHIPOBJ_USE_CODE_COVERAGE=ON` (built with ROCm's amdclang, reported on the
  run's summary page, and failed below the [coverage target](#tests); on
  `develop`, a job then publishes the result to the `badges` branch for the
  README's Coverage badge). It is the only workflow that builds these
  variants.
- Optional MinIO C++ bridge: `-DHIPOBJ_MINIO_CLIENT=ON`, plus the system
  packages listed in [docs/building.rst](docs/building.rst); see
  [integrations/minio-cpp/TESTING.md](integrations/minio-cpp/TESTING.md).

## Tests

- After a successful build: `cd build && ctest --output-on-failure`.
- The ctest suite doesn't need a GPU, an RDMA NIC, or an S3 server; CI runs all
  of it in a plain container. Tests that need more are skipped or not
  registered:
  - `v2-data-e2e` is only registered when a verbs device
    (`/dev/infiniband/uverbs*`) exists at configure time.
  - `hipobj-package-deb` and `hipobj-package-rpm` are skipped when the tools
    to build and read their packages (`dpkg`, or `rpmbuild` and `rpm`) aren't
    installed.
  - `hipobj-thread-safety-tags` and `hipobj-thread-safety-tags-selftest` are
    only registered when CMake finds Python 3.10 or later. CI's Build
    workflow skips changes to only their Python files, and
    [hipobject-pylint.yml](.github/workflows/hipobject-pylint.yml) runs them
    with Python 3.10 instead.
- The GPU-direct and RDMA data paths are tested against emulated hardware
  (rocm-ernic's ionic NIC and rocjitsu's GPU, under QEMU) by the
  `.github/workflows/hipobject-hardware-test-*.yml` workflows, and against real
  GPU hardware, RDMA NICs, and a compatible S3/cuObject lab; see
  [README.md](README.md) and [docs/interop.rst](docs/interop.rst).
- The sanitizer builds are part of the mandatory checks; see
  [C and C++](#c-and-c).
- **Code coverage: the library (`src/`) must keep at least 90% line coverage
  from `ctest`.** CI's coverage job fails below that. Add tests with new or
  changed code, including its error paths, rather than letting the number
  drift down; aim for most of the lines a change adds to be covered. To
  measure it, configure with Clang and `-DHIPOBJ_USE_CODE_COVERAGE=ON`, run
  `ctest`, and build the `hipobj-coverage` target, which writes
  `coverage/report.txt` and `coverage/lines.txt` in the build directory (see
  [docs/building.rst](docs/building.rst)). Tests of the public API run
  against the fake RDMA device and HIP runtime in
  [`test/unit/fake-device.h`](test/unit/fake-device.h), which records the
  calls and can make them fail.

## Public API

- **Don't change the public header, [`include/hipobj.h`](include/hipobj.h),
  without explicit permission.** hipObject is in development, so the API is
  expected to change, but only deliberately: don't rename, move, reorder, or
  restyle declarations as a side effect of other work. If a change seems to
  need a header change, ask first. Header changes are user-facing, so they also
  need a [changelog](#changelog) entry.
- **The public header is C**, and C11 and C++11 consumers both have to be able
  to include it. Keep it free of C++-isms and of anything newer than C11 or
  C++11. Every build with `BUILD_TESTING=ON`, including CI's, compiles it as
  C11 and as C++11 with `-Wall -Wextra -Werror -pedantic`
  ([`test/header`](test/header)), so a violation breaks the build.
- **The V2 API (hipobj-rc-v2) is experimental.** It can change in breaking
  ways or be removed at any time, so don't add compatibility shims or
  deprecation paths for it. Keep the V2 code, and any code that uses it (such
  as the tests and the MinIO bridge), behind `HIPOBJECT_V2_API`. CI also builds
  with `-DHIPOBJECT_V2_API=OFF`, and everything else has to build and work
  without it. The rules for the header and the changelog still apply to the V2
  declarations.
- **The examples in [`examples/`](examples/) show how consumers use the header,
  and are written to C++11.** The build compiles them as C++20 like everything
  else, so nothing catches newer constructs in them; keep them to C++11 by
  hand.

## Thread safety

The public header documents what callers can rely on, in its `threads` group
(the Thread Safety section of the API reference); read it before you add or
change a public function. These are the rules for keeping the code in line
with it:

- **Tag every public function `@serialized` or `@concurrent`** in its
  documentation comment. The tags are Doxygen aliases defined in
  [`docs/Doxyfile.in`](docs/Doxyfile.in).
- **A serialized function takes the API lock on its first line**, by creating
  a `hipObj::ApiGuard` ([`src/state.h`](src/state.h)), and returns
  `kReentryError` when the guard's `owns()` is false. Any function that reads
  or changes library state is serialized. A concurrent function doesn't take
  the lock, and doesn't touch library state or write to anything else shared,
  such as a `static` buffer. The `hipobj-thread-safety-tags` test
  ([`test/thread-safety`](test/thread-safety)) fails when a function's tag
  and its definition disagree.
- **Don't call a public function from inside the library.** The lock isn't
  recursive, so the call would get `kReentryError`.
- **State that the API lock protects doesn't need a lock of its own.**
- **Test concurrency changes with ThreadSanitizer** (see
  [C and C++](#c-and-c)), and add a new serialized function to the tests in
  [`test/unit/test-thread-safety.cpp`](test/unit/test-thread-safety.cpp),
  including the list of functions that a callback can't call.

## C++ code

- **The library, tests, and tools are C++20**, without GNU extensions
  (`HIPOBJ_CXX_STANDARD`, which also accepts 23).
- **Write modern C++.** hipObject is a C++ library with a C API, and only the
  public header has to look like C. Some existing code is C-style; don't copy
  it in new or changed code.
  - Use RAII to own resources: `std::unique_ptr` (or `std::shared_ptr` when
    ownership really is shared), not raw owning pointers, `new`/`delete`, or
    `malloc`/`free`. Wrap handles from C libraries (libibverbs, libcurl,
    `dlopen`) in a `std::unique_ptr` with a custom deleter or a small RAII
    class, so they're released on every path.
  - Use raw pointers only for non-owning access, such as the pointers the C
    API passes in and out.
  - Prefer the standard library and C++ language features to C idioms:
    `std::string` and `std::string_view` rather than `char` buffers;
    `std::array`, `std::vector`, and `std::span` rather than C arrays and
    pointer-and-length pairs; `std::optional` rather than sentinel values;
    `enum class`; `constexpr` rather than macros; `std::mutex` and
    `std::scoped_lock` rather than pthreads; and C++ casts rather than C casts.
  - Include the C++ versions of C standard library headers (`<cstdlib>`,
    `<cstring>`, `<cstdint>`, not `<stdlib.h>`, `<string.h>`, `<stdint.h>`),
    and use the `std::`-qualified names they declare. Only headers that C code
    includes, such as the public header, use the C names. POSIX and system
    headers (`<unistd.h>`, `<sys/mman.h>`, and so on) have no C++ versions.
  - Don't let exceptions cross the C API. The public functions in
    `src/hipobj.cpp` catch everything and return an error code; keep it that
    way.

## Portability

hipObject only builds on Linux today, but it may be built for Windows someday,
so don't add assumptions that only hold on Linux's data model.

- **Don't use `long` or `unsigned long` for sizes, offsets, or anything that
  has to be 64 bits.** `long` is 64 bits on Linux but 32 bits on Windows. Use
  `size_t` for sizes, `ptrdiff_t` for pointer differences, and the fixed-width
  types from `<cstdint>` (`int64_t`, `uint64_t`, `uint32_t`) for everything
  with a defined width, especially values that go on the wire.
- **Watch the APIs that are tied to `long`**: `strtol`/`strtoul` (use
  `std::from_chars`, or `strtoll`/`strtoull`), `%ld`/`%lu` format specifiers
  (use the `<cinttypes>` macros such as `PRIu64`, or `%zu` for `size_t`), and
  integer literal suffixes (`L`, `UL`).
- **`off_t` is a known exception.** The public header uses it for offsets, and
  that will be fixed when hipObject moves to rocm-systems. Until then, don't
  change it, and don't spread it into new internal code; use a fixed-width
  type there and convert, with a range check, at the API boundary.
- **Keep Linux-specific code behind the existing internal interfaces.** Don't
  scatter sysfs, `getrandom()`, `dlopen()`, or libibverbs calls through the
  library.

## Security

**Security is critical.** hipObject hands remote peers direct access to GPU and
host memory, and parses whatever the network sends back. A bug here can leak
memory contents or let a peer read or write memory it shouldn't, so security
takes priority over speed and convenience.

- **Fully initialize anything that touches the wire.** That covers tokens, HTTP
  headers and bodies, V2 control messages, and every verbs structure handed to
  libibverbs (work requests, scatter/gather entries, QP attributes). Initialize
  each one completely before filling it in (`T x{}`, or `std::memset` for C
  structs, as the existing code does). Serialize field by field; never copy a
  struct's raw bytes onto the wire, since its padding can carry stale memory,
  and `{}` doesn't guarantee that padding is zeroed.
- **Treat everything from the network as untrusted.** That includes S3
  responses, `x-amz-rdma-reply` values, peer tokens, V2 reply headers, and
  completions. Check lengths, character sets (hex, base64), and ranges before
  using a value, and reject malformed input with an error rather than
  truncating, clamping, or guessing. Never use a peer-supplied remote address,
  rkey, length, or offset without checking it against what was agreed.
- **Integer arithmetic must be safe.**
  - Check for wrap-around before it happens, not after. `offset + size`, size
    multiplications, and buffer-end calculations need an explicit overflow
    check (`__builtin_add_overflow`, `__builtin_mul_overflow`, or comparing
    against the limit first).
  - Check ranges before narrowing (for example, a `uint64_t` size into a
    32-bit verbs length, or anything into `int`). Use `std::in_range` and the
    `std::cmp_*` functions from `<utility>` for signed/unsigned comparisons;
    remember that `off_t` is signed and that `long`'s size depends on the
    platform (see [Portability](#portability)).
  - Avoid undefined behavior: signed overflow, shifts by a negative amount or
    by the type's width or more, and casts of out-of-range values.
  - UBSan catches signed overflow, but unsigned wrap-around is defined
    behavior, so no sanitizer reports it. That part is on you.
- **Bound every buffer.** No `strcpy`, `sprintf`, or unbounded `strcat`. Where
  the C API fills a caller's buffer (such as `hipObjTokenClientNic()`), respect
  its length and always NUL-terminate.
- **Grant the least memory access needed.** Register memory and set QP access
  flags with only the access the transfer requires, and don't widen existing
  flags without a reason.
- **Keep secrets secret.** Don't log, print, or put in error messages the S3
  credentials (`accessKey`, `secretKey`) or full RDMA tokens, which carry
  rkeys and addresses. Session IDs, cookies, and anything else that must be
  unpredictable come from the `getrandom()`-backed source in
  [`src/rdma/v2-random.h`](src/rdma/v2-random.h), never from `rand()` or a
  `<random>` engine.
- **Keep the hardening flags.** Don't remove or weaken `_FORTIFY_SOURCE`, the
  stack protector, `-fstack-clash-protection`, or `-z noexecstack`
  ([`cmake/`](cmake/)), and ask before changing any compile or link options
  (see [Project direction](#project-direction)).
- **Fix security scan findings.** CodeQL
  ([codeql.yml](.github/workflows/codeql.yml)) and the PR security scan
  ([pr-security-scan.yml](.github/workflows/pr-security-scan.yml): bandit,
  gitleaks, trivy, and zizmor) run on PRs, and their findings get fixed, not
  dismissed.

## Data path performance

**Keep the overhead on the hot data path low.** The payload moves by RDMA
without passing through the host, so what a transfer costs beyond the wire is
mostly what hipObject adds. The hot path is everything that runs once per
transfer: `hipObjGet()` and `hipObjPut()` (and their V2 counterparts), minting
and parsing RDMA tokens and replies, `hipObjBufSync()`, and posting and polling
RDMA work. `hipObjInit()`, buffer registration, and shutdown are setup, and can
afford more.

- **Do expensive work once, at setup**: opening devices, choosing the NIC,
  creating queue pairs, registering memory, exporting dmabufs, and reading
  sysfs or topology information. Don't move any of it onto the hot path.
- **Keep the hot path lean.** Avoid heap allocations, copying the payload (the
  host-staged fallback is the only path that copies it), string formatting and
  logging on success, system calls that aren't needed, and HIP calls that
  synchronize. Don't add locks without a reason, and hold them briefly. The
  API lock is the exception: it's held for a whole call by design (see
  [Thread safety](#thread-safety)).
- **Modern C++ is compatible with this.** `std::unique_ptr`, `std::span`, and
  `std::string_view` cost nothing over their C equivalents. Watch for the
  constructs that do cost something, such as copying a `std::shared_ptr`
  (an atomic reference count update), a `std::function` that allocates, or a
  `std::string` built per transfer.
- **Don't give up correctness or safety for speed.** If a change has to add
  work to the hot path, say so and explain why.

## Linters, warnings, and sanitizers (mandatory)

**Every change must pass every check below that covers the files it touches.
These checks are not optional or advisory.** A change with an outstanding
finding isn't finished, so don't hand it back. CI doesn't run all of these
checks, and several CI jobs only run when files matching a path filter change,
so a green PR doesn't prove a change is clean. Run the checks yourself.

Fix the code rather than suppressing a finding (`// NOLINT`,
`# shellcheck disable=`, `# pylint: disable=`, `-Wno-…`, and so on). Only
suppress a finding when it's unavoidable, for example because of how a
framework such as Google Test works, and comment why. Compiler warnings have
their own suppression mechanism, described under [C and C++](#c-and-c).

Run the commands below from the repository root, where the linters find their
configuration files.

### C and C++

- **clang-format 18**, with [`.clang-format`](.clang-format). Reformat with
  `util/format-source.sh 18`, or check without changing anything:

  ```bash
  git ls-files -z '*.c' '*.h' '*.cpp' '*.hpp' \
    | xargs -0 clang-format-18 --dry-run -Werror
  ```

  CI:
  [hipobject-format-check.yml](.github/workflows/hipobject-format-check.yml).
- **No compiler warnings, with both GCC and Clang.** They warn about different
  things, so build with each, in separate build directories
  (`-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`, and `clang`/`clang++`).
  CI builds with GCC, apart from its one code coverage job, so it doesn't check
  Clang's warnings across configurations. The build turns on a long list of
  warnings
  ([`HIPOBJGNUCompilerOptions.cmake`](cmake/HIPOBJGNUCompilerOptions.cmake),
  [`HIPOBJClangCompilerOptions.cmake`](cmake/HIPOBJClangCompilerOptions.cmake))
  but doesn't use `-Werror` unless it's configured with `-DHIPOBJ_WERROR=ON`,
  as CI's build workflow is, so configure with it on or read the build
  output.
- **Suppress a warning only when it's unavoidable**, and then only with the
  macros in [`shared/hipobj-warnings.h`](shared/hipobj-warnings.h), never with
  raw `#pragma`s, `-Wno-…` flags, or changes to the warning lists. Add an
  `HIPOBJ_WARN_<NAME>_OFF`/`_ON` pair for the specific warning there (defined
  empty for compilers that don't have it, like the existing pairs), add both to
  `StatementMacros` in [`.clang-format`](.clang-format), wrap the smallest
  region you can, and comment why the warning can't be fixed. `shared/` is
  already on the include path of the library, the unit tests, and
  `hipobj-rdma-test-server`; add it only to a target that lacks it (the
  examples, the MinIO bridge, `v2-client-harness`, and `v2-data-client`).
  Third-party code fetched with `FetchContent` is exempt; CMake
  already marks its headers `SYSTEM` and builds it with its warnings off.
- **clang-tidy**: configure with `-DHIPOBJ_USE_CLANG_TIDY=ON` and build; it
  must report nothing. CI doesn't run clang-tidy.
- **include-what-you-use (IWYU)**: configure with `-DHIPOBJ_USE_IWYU=ON` and
  build; IWYU prints the `#include` lines it suggests adding or removing for
  each file. Unlike the other checks, it doesn't have to report nothing, since
  some of its suggestions are wrong. Make the suggestions that are right for
  the files you touch, such as adding a header for a symbol the file uses but
  only gets indirectly. Ignore the ones that are wrong, and leave the code as
  it is. For example, IWYU asks for `<compare>` in files that compare
  `std::chrono` time points (`steady_clock::now() < deadline`), because
  libstdc++ implements those comparisons with `operator<=>`, but `<chrono>`
  already includes `<compare>`, as the standard requires. Other wrong
  suggestions IWYU makes here:
  - Removing `ibv-core.h` from a header that names the verbs types only as
    `struct ibv_pd *` and the like. Without a prior declaration, that
    declares a new `struct` in the enclosing namespace (`hipObj::ibv_pd`,
    not libibverbs' `::ibv_pd`), so keep the include.
  - Removing `<cstdint>` from `src/hipobj.cpp` because the public header
    includes `<stdint.h>`. C++ code includes `<cstdint>` (see
    [C++ code](#c-code)).
  - Adding `<memory>` "for allocator" where a file only uses containers.
  - Removing a forward declaration, with no text, at a lambda's line.
  - Naming a library's internal header rather than its public one. Fix these
    with a mapping in a `cmake/iwyu-*.imp` file instead, as
    [`cmake/iwyu-hip.imp`](cmake/iwyu-hip.imp) does for
    `<hip/driver_types.h>`, which only compiles after
    `<hip/hip_runtime_api.h>`.

  CI ([hipobject-iwyu.yml](.github/workflows/hipobject-iwyu.yml)) runs IWYU
  but only fails if IWYU itself stops working. It lists the suggestions in
  the job summary without failing.
- **Sanitizer-clean**: the tests must run without a report from
  AddressSanitizer, UndefinedBehaviorSanitizer, or ThreadSanitizer. Use one
  build directory per sanitizer, configured with `-DHIPOBJ_USE_SANITIZERS=ON`
  and `-DHIPOBJ_SANITIZER_TYPE=address`, `undefined`, or `thread`, then run
  `ctest`. UBSan reports an error and carries on by default, so a test can
  pass with a report in its output; run the UBSan build's tests with
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` so they fail instead. CI
  only runs AddressSanitizer.

### Shell

Shell scripts are bash, not POSIX sh: start them with `#!/usr/bin/env bash`,
and use `shell: bash` (or the workflow's `bash -e {0}` default) for `run:`
steps.

- **shellcheck** on every shell script:

  ```bash
  git ls-files -z '*.sh' | xargs -0 shellcheck
  ```

  CI: [hipobject-shellcheck.yml](.github/workflows/hipobject-shellcheck.yml),
  when a `.sh` file or the workflow file changes.
- **The shell embedded in YAML files must be shellcheck-clean too.** Every
  `run:` block in `.github/workflows/*.yml` is a bash script (the `hipobject-*`
  workflows set `defaults.run.shell` to `bash -e {0}`, and `codeql.yml` sets
  `shell: bash` on its one `run:` step), and the shellcheck workflow doesn't
  look at them. Check them with
  [actionlint](https://github.com/rhysd/actionlint) (`pip install
  actionlint-py` provides it), which runs shellcheck on each `run:` block, so
  shellcheck must be on `PATH`:

  ```bash
  actionlint -ignore 'specifying action "\$/'
  ```

  The `-ignore` is needed because actionlint (as of 1.7.12) rejects the
  self-repository syntax the hardware-test workflows use to load the in-repo
  action (`uses: $/.github/actions/fetch-guest-vm`); that report is spurious.
  Nothing in CI runs actionlint.

  actionlint doesn't read composite actions, so check the `run:` steps in
  `.github/actions/*/action.yml` by copying each one into a file with a bash
  shebang and running shellcheck on it. Neither tool sees the bodies of
  heredocs, such as the scripts the hardware-test workflows run inside the
  guest VMs, so hold those to the same standard by hand.

### CMake

- **cmakelint** 1.4.3, with [`.cmakelintrc`](.cmakelintrc):

  ```bash
  git ls-files -z 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' \
    | xargs -0 cmakelint
  ```

  CI: [hipobject-cmakelint.yml](.github/workflows/hipobject-cmakelint.yml),
  when a CMake file, `.cmakelintrc`, or the workflow file changes.

### Python

Python code must run on **Python 3.10**. Don't use anything newer, such as
`tomllib`, `except*`, or `typing.Self` (all 3.11).

- **black** and **pylint**, both targeting 3.10:

  ```bash
  git ls-files -z '*.py' | xargs -0 black --check --target-version py310
  git ls-files -z '*.py' | xargs -0 pylint --py-version=3.10
  ```

  CI: [hipobject-pylint.yml](.github/workflows/hipobject-pylint.yml), when a
  `.py` file or the workflow file changes. It pins both tools; use the same
  versions locally if black's output differs from CI's. Neither tool sees
  Python embedded in other files, such as the heredocs the hardware-test
  workflows pass to `python3`, so keep those black-formatted by hand.

- **vermin**, since black and pylint don't catch most uses of newer language
  features or modules (`pip install vermin`). It exits nonzero and prints
  `Target versions not met` if the code needs a newer Python:

  ```bash
  git ls-files -z '*.py' | xargs -0 vermin -t=3.10- --violations --no-tips
  ```

### Spelling and documentation

- **codespell**, with [`.codespellrc`](.codespellrc), over the whole tree,
  Markdown included: `codespell`. CI:
  [hipobject-spell-check.yml](.github/workflows/hipobject-spell-check.yml).
- **doc8** for reStructuredText: `doc8 docs/ --max-line-length 80`. The
  documentation must also build, which doesn't need ROCm:

  ```bash
  cmake -B build-docs -DHIPOBJ_DOCS_ONLY=ON -DHIPOBJ_BUILD_DOCS=ON
  cmake --build build-docs --target sphinx-html
  ```

  CI:
  [hipobject-documentation-check.yml](.github/workflows/hipobject-documentation-check.yml),
  when the documentation, the public header, `README.md`, `requirements.txt`,
  the documentation's build files, or the workflow file change.

### Ansible

- **ansible-lint** and a playbook syntax check, run from `ansible/` after
  installing the Galaxy collection (see [ansible/README.md](ansible/README.md));
  [hipobject-ansible.yml](.github/workflows/hipobject-ansible.yml) has the exact
  commands. CI runs them when something under `ansible/` or the workflow file
  changes.

## Changelog

**Every user-facing change needs an entry in [CHANGELOG.md](CHANGELOG.md),
in the same change.** User-facing means anything someone building, installing,
or calling hipObject could notice: the public header and API, library behavior,
CMake options and variables, the installed files and packages, dependencies and
supported platforms, and the examples. CI-only, test-only, and internal
refactoring changes don't need an entry.

Add the entry to the topmost (unreleased) version, under the matching heading:
`Added`, `Changed`, `Removed`, or `Known issues` (if none fits, add one such as
`Resolved issues`, as other ROCm changelogs do). Write it for users, like the
existing entries: say what changed, and what users have to do differently, if
anything.

## Lab provisioning

For GPU hosts, ROCm, RDMA, and MinIO AIStor installs, use
[ansible/README.md](ansible/README.md).
