API Reference
=============

This page documents the hipObject public C API,
extracted from annotated source headers by Doxygen and
rendered via Breathe.

Core Functionality
------------------

.. doxygengroup:: core
   :content-only:
   :members:

Errors and Error Handling
-------------------------

.. doxygengroup:: error
   :content-only:
   :members:

Buffer Registration
-------------------

.. doxygengroup:: buffer
   :content-only:
   :members:

Data Transfer (GET / PUT)
-------------------------

.. doxygengroup:: io
   :content-only:
   :members:

Experimental V2 API (hipobj-rc-v2)
----------------------------------

.. warning::

   **The V2 API is experimental. It may change
   incompatibly or be removed in any release.**
   ``hipObjGetV2()`` and ``hipObjPutV2()`` are not
   implemented yet and return ``hipObjNotSupported``.

The V2 API runs each transfer over the hipobj-rc-v2
control protocol: two round trips on a dedicated control
endpoint, PREPARE and then READY, whose response is
FINAL. CANCEL abandons a prepared transfer.

These declarations, and the ``hipObjNotSupported`` and
``hipObjBusy`` error codes, only exist when
``HIPOBJECT_V2_API`` is defined. The ``HIPOBJECT_V2_API``
CMake option (``ON`` by default) builds the V2 API and
defines the macro for everything that links
``hipobj::hipobj``.

.. doxygengroup:: v2
   :content-only:
   :members:
