hipObject cuObject Interoperability
=====================================

This guide summarizes compatibility with `NVIDIA cuObject v1.2.0`_ and
documents how to test hipObject on lab hardware.

.. _NVIDIA cuObject v1.2.0: https://docs.nvidia.com/gpudirect-storage/cuobject/index.html

Compatibility matrix
--------------------

+---------------------------+------------------+------------------+
| Feature                   | cuObject v1.2.0  | hipObject        |
+===========================+==================+==================+
| S3 ``x-amz-rdma-token``   | Yes              | Yes              |
+---------------------------+------------------+------------------+
| S3 ``x-amz-rdma-reply``   | Yes              | Yes              |
+---------------------------+------------------+------------------+
| GET / PUT / multipart     | Yes              | Yes (via SDK)    |
+---------------------------+------------------+------------------+
| RDMA transport            | DC (ConnectX)    | RC (bnxt/ionic)  |
+---------------------------+------------------+------------------+
| Direct cuObject server    | N/A              | Requires adapter |
+---------------------------+------------------+------------------+

Control plane
~~~~~~~~~~~~~

hipObject is wire-compatible with cuObject at the S3 header level.
Applications embed a hex-encoded RDMA token in ``x-amz-rdma-token`` and
read ``x-amz-rdma-reply`` from the HTTP response.  Reply values may be
legacy tags (``ok`` / ``err``), numeric HTTP codes (``200``, ``501``),
or an RC peer handshake of the form ``200:<server-token-hex>``.

Data plane
~~~~~~~~~~

cuObject v1.2.0 requires Dynamic Connection (DC) transport on NVIDIA
ConnectX NICs.  hipObject uses Reliable Connection (RC) on Broadcom
Thor-2 and AMD Pensando Pollara NICs.  The transport byte in the token
distinguishes DC (``0x00``) from RC (``0x01``).

A stock ``libcuobjserver`` stack cannot serve RC clients directly.  An
RC-to-DC adapter (external to this repository) bridges AMD RC clients to
cuObject-equipped storage gateways in production deployments.

Testing paths
-------------

Layer 1 — Unit tests (CI)
~~~~~~~~~~~~~~~~~~~~~~~~~

Token encoding, reply parsing, and header formatting run in GitHub
Actions without GPU or RDMA hardware:

.. code-block:: bash

   cmake -B build -DBUILD_TESTING=ON
   cmake --build build
   ctest --test-dir build -R test-rdma-token --output-on-failure

Layer 2 — RC integration test server
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``hipobj-rdma-test-server`` is an in-repo S3+RDMA harness that speaks RC
and returns peer tokens for the client-side QP handshake:

.. code-block:: bash

   cmake -B build -DBUILD_TESTING=ON
   cmake --build build --target hipobj-rdma-test-server
   ./build/test/integration/rdma-test-server/hipobj-rdma-test-server 9000

On a GPU client node with libcurl installed:

.. code-block:: bash

   cmake -B build -DBUILD_TESTING=ON
   cmake --build build --target get-object
   ./build/hipobj-examples/get-object 4096 --live http://127.0.0.1:9000

Layer 3 — MinIO AIStor bridge
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Build the minio-cpp bridge for production-style S3 signing and retries:

.. code-block:: bash

   cmake -B build -DHIPOBJ_MINIO_CLIENT=ON
   cmake --build build --target minio-getput-rdma

See ``integrations/minio-cpp/TESTING.md`` for AIStor + cuObjServer setup.

Layer 4 — cuObject server probe (optional)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When ``libcuobjserver`` is installed locally:

.. code-block:: bash

   cmake -B build \
     -DHIPOBJ_FIND_CUOBJECT_SERVER=ON \
     -DCUOBJSERVER_ROOT=/opt/nvidia/cuobjserver \
     -DBUILD_TESTING=ON
   cmake --build build --target hipobj-cuobject-probe

This verifies linkage only.  End-to-end cuObject interop still requires
ConnectX hardware and the RC adapter on the storage gateway.

Hardware topology
-----------------

.. code-block:: text

   AMD GPU client                Storage server
   +----------------+            +------------------+
   | MI300 + hipObj |  RoCEv2 RC  | RC adapter       |
   | bnxt / ionic   | ----------> | cuObjServer (DC) |
   +----------------+            | MinIO AIStor     |
                                 +------------------+

CMake options
-------------

``HIPOBJ_FETCH_CUOBJECT_CLIENT``
  Download ``libcuobjclient`` v1.2.0.59 from the NVIDIA CUDA CDN.

``HIPOBJ_FIND_CUOBJECT_SERVER``
  Locate a system ``libcuobjserver`` and build ``hipobj-cuobject-probe``.

``HIPOBJ_INTEGRATION_TESTS``
  Build ``hipobj-rdma-test-server`` (default ON when testing is enabled).

``HIPOBJ_MINIO_CLIENT``
  Build the minio-cpp RDMA bridge and ``minio-getput-rdma`` example.
