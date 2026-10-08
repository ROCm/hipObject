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

Thread Safety
-------------

.. doxygengroup:: threads
   :desc-only:
   :no-title:

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

.. This warning is a copy of the one in hipobj.h, which
   Doxygen leaves out (it's in an @if HIPOBJ_HEADER_ONLY
   block) because Breathe would move it to the end of the
   description. Keep the two in sync. The rest of the
   description comes from the header, in two directives,
   since :content-only: leaves it out.

.. doxygengroup:: v2
   :desc-only:
   :no-title:

.. doxygengroup:: v2
   :content-only:
   :members:
