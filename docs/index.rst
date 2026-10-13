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
  headers.
- **Data plane**: RDMA READ/WRITE over RoCEv2 RC
  transport directly between GPU VRAM and storage server
  buffers.

The experimental V2 API replaces the control plane with
hipObject's own protocol, hipobj-rc-v2, on a dedicated
control endpoint. It uses the same RC data plane, and
isn't implemented yet. See :doc:`protocols`.

Supported S3 Operations
-----------------------

============  ================================
Operation     Description
============  ================================
``GET``       Fetch object to GPU VRAM
``PUT``       Store GPU VRAM to object
``UPLOAD``    Chunked upload (multipart)
``RANGE``     Byte-range fetch from object
============  ================================

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   building
   protocols
   interop

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api

License
-------

MIT License
