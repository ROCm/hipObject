hipObject: RDMA-Accelerated S3 Object Storage for GPUs
======================================================

.. warning::

   **hipObject is an experimental library. It is not
   intended for production use.**

Introduction
------------

hipObject enables direct data transfers between AMD GPU
VRAM and S3-compatible object storage using RDMA over
RoCEv2. It interoperates with NVIDIA cuObject-equipped
storage servers via the ``x-amz-rdma-token`` S3 header
protocol, providing a vendor-neutral client for
GPU-direct object storage.

Architecture
------------

hipObject separates control and data planes:

- **Control plane**: Standard S3 REST requests augmented
  with ``x-amz-rdma-token`` / ``x-amz-rdma-reply``
  headers, made by the application's S3 client.
- **Data plane**: RDMA READ/WRITE over RoCEv2 RC
  transport directly between GPU VRAM and storage server
  buffers.

See :doc:`architecture` for how the library works.

Transfers
---------

hipObject doesn't make S3 requests. The application's S3
client sends each request, with an RDMA token from
hipObject in its ``x-amz-rdma-token`` header, and the
server moves the data by RDMA:

.. list-table::
   :header-rows: 1
   :widths: 20 45 35

   * - Call
     - The application's S3 request
     - The server
   * - ``hipObjGet()``
     - GetObject, for a whole object or a byte range
     - RDMA WRITEs into the buffer
   * - ``hipObjPut()``
     - PutObject, or UploadPart of a multipart upload
     - RDMA READs from the buffer

The request selects the object, and the part of it, to
transfer; the token only describes the buffer. The buffer
can be in GPU or host memory.

Getting Started
---------------

:doc:`building` covers building and installing hipObject,
and using it from a CMake project. The README's
`Quick Start`_ is a short program that reads an object into
GPU memory, and the `examples`_, which are installed in
``share/doc/hipobject<mm>/examples``, are complete programs
that make their S3 requests with libcurl.

.. _Quick Start: https://github.com/ROCm/hipObject/blob/develop/README.md#quick-start
.. _examples: https://github.com/ROCm/hipObject/tree/develop/examples

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   building
   architecture
   interop
   rc-rdma-flow

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api

License
-------

MIT License
