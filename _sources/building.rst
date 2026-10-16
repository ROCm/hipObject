Building hipObject
==================

Prerequisites
-------------

Required
^^^^^^^^

- CMake 3.25.2 or later
- ROCm 7.14 or later (provides HIP, HSA runtime)
- C++20 capable compiler (e.g., g++ or amdclang++ from ROCm)

Optional
^^^^^^^^

- rdma-core 49 or later development headers
  (``libibverbs-dev`` or ``rdma-core-devel``), for the test
  that checks hipObject's copies of the libibverbs structures
  against them. The library doesn't need them, since it loads
  libibverbs with ``dlopen``.
- GTest (fetched automatically if not found)
- Doxygen, Python 3, Sphinx, Breathe (for documentation)

Run Time
^^^^^^^^

RDMA needs libibverbs and the libibverbs provider for the
NIC, from rdma-core: ``libibverbs1`` and ``ibverbs-providers``
on Ubuntu, or ``libibverbs`` on RHEL. hipObject loads
``libibverbs.so.1`` with ``dlopen`` the first time
``hipObjInit()`` runs, so it doesn't need the development
package. Without libibverbs, ``hipObjInit()`` prints why
libibverbs didn't load and fails with ``hipObjRdmaError``.

Build Steps
-----------

.. code-block:: bash

   mkdir build && cd build
   cmake .. \
     -DCMAKE_BUILD_TYPE=Release \
     -DHIPOBJ_BNXT=ON \
     -DHIPOBJ_IONIC=OFF

   make -j$(nproc)

   # Run tests (requires GPU + RDMA NIC)
   ctest --output-on-failure

   # Install (to ROCM_PATH)
   sudo make install

CMake Options
-------------

================================ ============== =============================
Option                           Default        Description
================================ ============== =============================
``CMAKE_BUILD_TYPE``             RelWithDebInfo Build type (see below)
``HIPOBJ_CXX_STANDARD``          20             C++ standard (20, 23, 26)
``HIPOBJ_BNXT``                  ON             Build Thor-2 RDMA backend
``HIPOBJ_IONIC``                 ON             Build ionic RDMA backend
``BUILD_SHARED_LIBS``            ON             Build shared library
``BUILD_TESTING``                ON             Build and register tests
``HIPOBJ_INSTALL_EXAMPLES``      ON             Build examples (see below)
``HIPOBJ_INSTALL_TOOLS``         ON             Build tools (see below)
``HIPOBJ_BUILD_DOCS``            OFF            Build documentation
``HIPOBJ_DOCS_ONLY``             OFF            Docs targets only (no HIP)
``HIPOBJ_MINIO_CLIENT``          OFF            Build minio-cpp RDMA bridge
``HIPOBJ_INTEGRATION_TESTS``     ON             Build RC test server
``HIPOBJ_FETCH_CUOBJECT_CLIENT`` OFF            Fetch libcuobjclient 1.2.0.59
``HIPOBJ_FIND_CUOBJECT_SERVER``  OFF            Find libcuobjserver + probe
``HIPOBJ_WERROR``                OFF            Treat warnings as errors
``HIPOBJ_USE_CODE_COVERAGE``     OFF            Build with code coverage
``ROCM_PATH``                    see below      Path to ROCm install
``ROCM_VERSION``                 detected       ROCm version
================================ ============== =============================

``CMAKE_BUILD_TYPE`` can be ``Debug``, ``Release``,
``RelWithDebInfo``, or ``None``. ``RelWithDebInfo`` and ``Release``
builds are optimized and compile out the library's ``assert()``
checks. They also enable ``_FORTIFY_SOURCE=3`` when building with
Clang or GCC 12 and later. Use ``Debug`` when
developing hipObject or running it under a sanitizer. Multi-config
generators, such as Ninja Multi-Config, ignore ``CMAKE_BUILD_TYPE``
and pick the configuration at build time.

``HIPOBJ_CXX_STANDARD`` selects the C++ standard that hipObject is
built with, without compiler extensions. ``26`` needs CMake 3.30 or
later, which is the first version that knows how to ask GCC
(14 and later) and Clang for C++26.

``HIPOBJ_INSTALL_EXAMPLES`` builds the example programs in the
build tree (``examples/``) and installs their sources, with a
``CMakeLists.txt`` that builds them against the installed hipObject,
in ``share/doc/hipobject<mm>/examples`` (see `Packaging`_). The
example programs aren't installed. The ``README.md`` installed with
them explains how to build and run them.

``HIPOBJ_INSTALL_TOOLS`` builds and installs the tool programs, in
``bin``, in the runtime package (see `Packaging`_). So far there's
one, ``hipobj-test-install``, which will check that hipObject is
installed and works, as far as the system's hardware allows. For
now it only reports the hipObject version, with ``--version``.

``HIPOBJ_WERROR`` adds ``-Werror`` to hipObject's own targets
(the library, tests, examples, and tools), but not to third-party
code that CMake fetches. It's off by default so that a compiler
hipObject hasn't been tested with can't break the build with a new
warning. CI turns it on, and developers should too.

``HIPOBJ_USE_CODE_COVERAGE`` builds hipObject with LLVM's
source-based code coverage instrumentation. It needs Clang (or
ROCm's ``amdclang``) as both the C and C++ compiler, and the
``llvm-profdata`` and ``llvm-cov`` from the same LLVM version, which
CMake looks for next to the compiler. Set ``HIPOBJ_LLVM_PROFDATA``
and ``HIPOBJ_LLVM_COV`` if it doesn't find them. Run the tests, then
build the ``hipobj-coverage`` target to report the coverage of the
library's sources:

.. code-block:: bash

   cmake -B build -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
     -DHIPOBJ_USE_CODE_COVERAGE=ON
   cmake --build build
   (cd build && ctest)
   cmake --build build --target hipobj-coverage

The report goes in ``coverage/report.txt`` (a summary for each file)
and ``coverage/lines.txt`` (the coverage of each line) in the build
directory. Every instrumented program writes its profile to
``coverage/profraw`` in the build directory, however it's run, and
the profiles add up over runs, so delete ``coverage/profraw`` to start
over. Since the programs write to the build directory, don't install
or package a coverage build.

``ROCM_PATH`` and ``ROCM_VERSION`` can also be set in the
environment. If ``ROCM_PATH`` isn't set but ``ROCM_VERSION`` is,
``ROCM_PATH`` defaults to ``/opt/rocm/core-<major>.<minor>``. If
neither is set, ``ROCM_PATH`` defaults to ``/opt/rocm/core`` if it
contains a ROCm installation, and otherwise to ``/opt/rocm``.

``ROCM_VERSION`` defaults to the version in
``ROCM_PATH``/.info/version, or in ``ROCM_PATH``/core/.info/version
when ``ROCM_PATH`` is ``/opt/rocm``, which only contains links.
Configuring fails if ``ROCM_VERSION`` is earlier than 7.14.

Packaging
---------

hipObject uses `rocm-cmake <https://github.com/ROCm/rocm-cmake>`_
to install and package the library. If ROCm doesn't provide
rocm-cmake, it is downloaded when hipObject is configured. To
build DEB or RPM packages, run CPack in the build directory:

.. code-block:: bash

   cpack -G DEB    # or RPM

========================= ========================= ==========================
DEB package               RPM package               Contents
========================= ========================= ==========================
``hipobject<mm>``         ``hipobject<mm>``         Shared library, license,
                                                    tools
``hipobject-dev<mm>``     ``hipobject-devel<mm>``   Header, CMake package,
                                                    ``libhipobj.so`` link,
                                                    examples' sources
========================= ========================= ==========================

A static build (``BUILD_SHARED_LIBS=OFF``) creates a single
``hipobject-static-dev<mm>`` DEB or
``hipobject-static-devel<mm>`` RPM package, without the tools.
Test programs aren't packaged. With ``HIPOBJ_MINIO_CLIENT``, the
runtime package also has the ``minio-getput-rdma`` program (see
`MinIO C++ RDMA Bridge`_).

``<mm>`` is the major and minor version of the ROCm that
hipObject is built for, as in ROCm's own package names, so the
runtime package for ROCm 10.1 is ``hipobject10.1``. Builds for
different ROCm versions are different packages, which install
side by side when each installs into its ROCm version's own
directory, ``/opt/rocm/core-<mm>``, as they do when
``ROCM_VERSION`` is set (see `CMake Options`_) and as the
nightly packages do.

The package version is hipObject's version followed by the ROCm
version with each ``.`` replaced by ``0``, such as
``0.1.0.100100`` for ROCm 10.1.0, and a release. The release
comes from the ``CPACK_DEBIAN_PACKAGE_RELEASE`` or
``CPACK_RPM_PACKAGE_RELEASE`` environment variable when
hipObject is configured, and is ``local`` if it isn't set. An
RPM package's release also ends with the distribution's tag,
such as ``.el9``, when the build machine's ``rpm`` defines one
(``%{?dist}``).

CPack builds DEB packages with ``dpkg-deb``, and RPM packages
with ``rpmbuild``. rocm-cmake only sets up the generators whose
tools it finds when hipObject is configured.

Package Dependencies
^^^^^^^^^^^^^^^^^^^^

The packages declare these dependencies. RPM calls Depends
"Requires". Replaces is Conflicts and Replaces in a DEB
package, and Obsoletes in an RPM package.

.. list-table::
   :header-rows: 1
   :widths: 22 14 32 32

   * - Package
     - Relationship
     - DEB
     - RPM
   * - ``hipobject<mm>``
     - Depends
     - ``amdrocm-runtime-dev<mm>``, the MinIO program's libraries
     - ``amdrocm-runtime-devel<mm>``, the MinIO program's libraries
   * -
     - Recommends
     - ``libibverbs1``, ``hipobject-dev<mm>``
     - ``libibverbs.so.1()(64bit)`` (CMake 4.1 or later)
   * -
     - Suggests
     -
     - ``hipobject-devel<mm>`` (CMake 4.0 or later)
   * -
     - Replaces
     - ``hipobject``
     - ``hipobject``
   * - ``hipobject-dev<mm>`` or ``hipobject-devel<mm>``
     - Depends
     - ``hipobject<mm> (= <version>-<release>)``
     - ``hipobject<mm> = <version>-<release>``
   * -
     - Replaces
     - ``hipobject-dev``
     - ``hipobject-devel``
   * - ``hipobject-static-dev<mm>`` or
       ``hipobject-static-devel<mm>``
     - Depends
     - ``amdrocm-runtime-dev<mm>``
     - ``amdrocm-runtime-devel<mm>``
   * -
     - Recommends
     - ``libibverbs1``
     - ``libibverbs.so.1()(64bit)`` (CMake 4.1 or later)
   * -
     - Replaces
     - ``hipobject-static-dev``
     - ``hipobject-static-devel``

The development package depends on the runtime package with its
own version and release, so the header and the library always
come from the same build. Each package replaces the package it
was named before it had the ROCm version, which only nightly
packages had, so installing it removes that package.

The MinIO program's libraries, with ``HIPOBJ_MINIO_CLIENT``, are
libcurl, OpenSSL, zlib, and the system's pugixml and curlpp, if
it uses them. The RPM packages name libraries by soname, which
every distribution's library packages provide. The static package
doesn't have the program, so it doesn't depend on its libraries.
The examples' sources need libcurl's development package to build
their ``--live`` mode, but the packages don't depend on it.

The package that holds the library recommends libibverbs,
which RDMA needs (see `Run Time`_). It's a recommendation
rather than a dependency so that hipObject can be installed
without it, such as on a build machine. ``apt`` and ``dnf``
install recommended packages by default, but not suggested
ones. hipObject doesn't include libibverbs in its packages.
rocm-cmake adds the runtime package's recommendation or
suggestion of the development package.

DEB packages get every dependency in the table with any
supported CMake. RPM packages only get the weak dependencies
(Recommends and Suggests) that the CMake that builds them
supports:

============== ========================= ========================
CMake          RPM Recommends            RPM Suggests
============== ========================= ========================
Before 4.0     No                        No
4.0            No                        Yes
4.1 or later   Yes                       Yes
============== ========================= ========================

CPack's RPM generator before CMake 4.0 drops every weak
dependency, since its check for ``rpmbuild``'s support of them
doesn't work, and Recommends needs 4.1. Build RPM packages
with CMake 4.1 or later to get them all.

Nightly Packages
^^^^^^^^^^^^^^^^

DEB packages of the ``develop`` branch are built every night
that it changes, and published as the `nightly prerelease
<https://github.com/ROCm/hipObject/releases/tag/nightly>`_,
which each new build replaces. There are packages for Ubuntu
24.04 and 26.04, built for ROCm 10.1.0 and 7.14.1. They
install into ``/opt/rocm/core-<major>.<minor>``, so the
packages for both ROCm versions can be installed together, and
their release is
``0.<commit time>.g<commit>~<Ubuntu release>``, so a newer
nightly upgrades an older one. The nightly packages are for
testing, and aren't a supported release.

MinIO C++ RDMA Bridge
---------------------

Build the ``hipobj_minio`` library and ``minio-getput-rdma``
example with ``HIPOBJ_MINIO_CLIENT``:

.. code-block:: bash

   cmake -B build \
     -DCMAKE_BUILD_TYPE=Release \
     -DHIPOBJ_MINIO_CLIENT=ON

   cmake --build build

Additional system packages (Ubuntu 24.04):

.. code-block:: bash

   sudo apt install libssl-dev zlib1g-dev libcurl4-openssl-dev

See ``integrations/minio-cpp/TESTING.md`` for lab validation
against MinIO AIStor over RDMA.

cuObject Interoperability
-------------------------

See :doc:`interop` for the v1.2.0 compatibility matrix. Build
the in-repo RC test server:

.. code-block:: bash

   cmake -B build -DBUILD_TESTING=ON

   cmake --build build --target hipobj-rdma-test-server

Optional cuObject library probes:

.. code-block:: bash

   cmake -B build \
     -DHIPOBJ_FETCH_CUOBJECT_CLIENT=ON \
     -DHIPOBJ_FIND_CUOBJECT_SERVER=ON \
     -DCUOBJSERVER_ROOT=/opt/nvidia/cuobjserver

Building Documentation
----------------------

To build the HTML documentation locally:

.. code-block:: bash

   cmake -B build \
     -DHIPOBJ_BUILD_DOCS=ON

   cmake --build build --target hipobj-sphinx-html

The output appears in ``build/docs/html/``.

For a docs-only build that does not require a ROCm/HIP
toolchain:

.. code-block:: bash

   cmake -B build \
     -DHIPOBJ_DOCS_ONLY=ON \
     -DHIPOBJ_BUILD_DOCS=ON

   cmake --build build --target hipobj-sphinx-html

Environment Variables
---------------------

- ``ROCM_PATH``: Override the ROCm installation path.
- ``ROCM_VERSION``: Override the ROCm version.
- ``LD_LIBRARY_PATH``: Must include ROCm and rdma-core
  library paths at runtime.
