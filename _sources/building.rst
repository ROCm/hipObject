Building hipObject
==================

Prerequisites
-------------

Required
^^^^^^^^

- CMake 3.21 or later
- ROCm 6.x or later (provides HIP, HSA runtime)
- C++20 capable compiler (e.g., g++ or amdclang++ from ROCm)

Optional
^^^^^^^^

- rdma-core development headers (for system libibverbs;
  hipObject loads libibverbs via ``dlopen`` so this is
  not strictly required at build time)
- GTest (fetched automatically if not found)
- Doxygen, Python 3, Sphinx, Breathe (for documentation)

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
``HIPOBJ_BNXT``                  ON             Build Thor-2 RDMA backend
``HIPOBJ_IONIC``                 ON             Build ionic RDMA backend
``BUILD_SHARED_LIBS``            ON             Build shared library
``BUILD_TESTING``                ON             Build and register tests
``HIPOBJ_BUILD_DOCS``            OFF            Build documentation
``HIPOBJ_DOCS_ONLY``             OFF            Docs targets only (no HIP)
``HIPOBJ_MINIO_CLIENT``          OFF            Build minio-cpp RDMA bridge
``HIPOBJ_INTEGRATION_TESTS``     ON             Build RC test server
``HIPOBJ_FETCH_CUOBJECT_CLIENT`` OFF            Fetch libcuobjclient 1.2.0.59
``HIPOBJ_FIND_CUOBJECT_SERVER``  OFF            Find libcuobjserver + probe
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

``ROCM_PATH`` and ``ROCM_VERSION`` can also be set in the
environment. If ``ROCM_PATH`` isn't set but ``ROCM_VERSION`` is,
``ROCM_PATH`` defaults to ``/opt/rocm/core-<major>.<minor>`` for
ROCm 7.11 and later, and to ``/opt/rocm-<version>`` for earlier
releases. If neither is set, ``ROCM_PATH`` defaults to
``/opt/rocm/core`` if it contains a ROCm installation, and
otherwise to ``/opt/rocm``.

``ROCM_VERSION`` defaults to the version in
``ROCM_PATH``/.info/version, or in
``ROCM_PATH``/core/.info/version for the ROCm 7.11 and later
layout, where ``/opt/rocm`` only contains links.

Packaging
---------

hipObject uses `rocm-cmake <https://github.com/ROCm/rocm-cmake>`_
to install and package the library. If ROCm doesn't provide
rocm-cmake, it is downloaded when hipObject is configured. To
build DEB or RPM packages, run CPack in the build directory:

.. code-block:: bash

   cpack -G DEB    # or RPM

=================== ===================== ==============================
DEB package         RPM package           Contents
=================== ===================== ==============================
``hipobject``       ``hipobject``         Shared library, examples,
                                          license
``hipobject-dev``   ``hipobject-devel``   Header, CMake package,
                                          ``libhipobj.so`` link
=================== ===================== ==============================

A static build (``BUILD_SHARED_LIBS=OFF``) creates a single
``hipobject-static-dev`` DEB or ``hipobject-static-devel`` RPM
package, without the examples. The package version includes
the ROCm version. Test programs aren't packaged.

The ``hipobject`` package depends on ROCm and on the system
libraries the examples link, such as libcurl (and OpenSSL,
zlib, and pugixml with ``HIPOBJ_MINIO_CLIENT``). The RPM
package requires these libraries by soname (e.g.,
``libcurl.so.4()(64bit)``).

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

   cmake --build build --target sphinx-html

The output appears in ``build/docs/html/``.

For a docs-only build that does not require a ROCm/HIP
toolchain:

.. code-block:: bash

   cmake -B build \
     -DHIPOBJ_DOCS_ONLY=ON \
     -DHIPOBJ_BUILD_DOCS=ON

   cmake --build build --target sphinx-html

Environment Variables
---------------------

- ``ROCM_PATH``: Override the ROCm installation path.
- ``ROCM_VERSION``: Override the ROCm version.
- ``LD_LIBRARY_PATH``: Must include ROCm and rdma-core
  library paths at runtime.
