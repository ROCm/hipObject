Transfer Protocols
==================

hipObject has two APIs for moving data, and they differ in their
*control protocol*: how the client and the storage server agree on a
transfer before the data moves. They don't differ in their RDMA
transport. Both move the data with RDMA over RoCEv2 using Reliable
Connection (RC) transport.

* ``hipObjGet()`` and ``hipObjPut()`` use the **cuObject-compatible
  protocol**, which exchanges RDMA tokens in headers of the
  application's own S3 request and its response. This is the API to
  use.
* ``hipObjGetV2()`` and ``hipObjPutV2()``, the **experimental V2 API**,
  use **hipobj-rc-v2**, hipObject's own two-round-trip protocol. They
  aren't implemented yet.

The "V2" in the second API's name doesn't make it a newer version of
``hipObjGet()`` and ``hipObjPut()``, and the "rc" in ``hipobj-rc-v2``
doesn't mean it's the RC API. It's an alternative control protocol over
the same transport.

RC and DC transport
-------------------

hipObject only implements RC transport. NVIDIA cuObject servers use
Dynamic Connection (DC) transport, which only NVIDIA ConnectX NICs
support, so an RC-to-DC adapter on the server bridges hipObject clients
to them (see :doc:`interop`).

DC shows up in hipObject in one place: the RDMA token format, which comes
from cuObject, has a transport byte whose values are DC (``0x00``) and
RC (``0x01``). The tokens hipObject creates are always RC.

Comparison
----------

+-------------------+-------------------------+-------------------------+
|                   | ``hipObjGet()`` /       | ``hipObjGetV2()`` /     |
|                   | ``hipObjPut()``         | ``hipObjPutV2()``       |
+===================+=========================+=========================+
| Control protocol  | cuObject-compatible     | hipobj-rc-v2            |
|                   | S3 headers              |                         |
+-------------------+-------------------------+-------------------------+
| Where it runs     | The application's own   | A dedicated control     |
|                   | S3 GET or PUT request   | endpoint, separate from |
|                   |                         | the S3 endpoint         |
+-------------------+-------------------------+-------------------------+
| Round trips       | One                     | Two: PREPARE, then      |
|                   |                         | READY                   |
+-------------------+-------------------------+-------------------------+
| RDMA transport    | RC                      | RC                      |
+-------------------+-------------------------+-------------------------+
| Queue pair        | One, created by         | One per transfer, by    |
|                   | ``hipObjInit()`` and    | design, released when   |
|                   | used by every transfer  | the call returns        |
+-------------------+-------------------------+-------------------------+
| Which object      | Chosen by the           | Named in the call:      |
|                   | application's request;  | bucket, key, and an     |
|                   | hipObject only sees the | optional query string   |
|                   | buffer                  |                         |
+-------------------+-------------------------+-------------------------+
| ``offset``        | Byte offset into the    | Byte offset into the    |
| argument          | registered buffer       | object                  |
+-------------------+-------------------------+-------------------------+
| Callbacks         | ``hipObjOps_t``:        | ``hipObjOpsV2_t``:      |
|                   | ``sendRequest``,        | ``sendPrepare``,        |
|                   | ``recvReply``           | ``sendReady``,          |
|                   |                         | ``sendCancel``          |
+-------------------+-------------------------+-------------------------+
| Server support    | cuObject servers,       | Servers that implement  |
|                   | through the RC-to-DC    | hipobj-rc-v2; cuObject  |
|                   | adapter                 | servers don't           |
+-------------------+-------------------------+-------------------------+
| Status            | Supported               | Experimental; not       |
|                   |                         | implemented             |
+-------------------+-------------------------+-------------------------+

The cuObject-compatible protocol
--------------------------------

``hipObjGet()`` and ``hipObjPut()`` piggyback on an S3 request that the
application makes with its own S3 client, through the ``hipObjOps_t``
callbacks. The server moves the data with RDMA: it writes the buffer for
a GET and reads it for a PUT. On the client:

#. ``hipObjInit()`` creates the one RC queue pair that every transfer
   uses.
#. For each transfer, hipObject creates an RDMA token describing the
   buffer and passes it to the ``sendRequest`` callback, which sends it
   in the ``x-amz-rdma-token`` header of the S3 GET or PUT request.
#. The ``recvReply`` callback returns the ``x-amz-rdma-reply`` header
   from the response. When it carries the server's own token
   (``200:<server-token-hex>``), hipObject connects the queue pair to
   it.
#. hipObject waits for the transfer's completion.

The application's request chooses the object, the part of it to
transfer, and the endpoint, so hipObject doesn't need to know any of
them. The ``offset`` argument is a byte offset into the registered
buffer, not into the object.

An application that can't use callbacks can drive the same exchange
itself with ``hipObjGetRdmaToken()``, ``hipObjBufSync()``, and
``hipObjParseRdmaReply()``. In ``hipObjGetRdmaToken()`` and
``hipObjPutRdmaToken()``, "Get" and "Put" mean create and release a
token, not S3 GET and PUT.

:doc:`interop` describes the token and reply formats.

The hipobj-rc-v2 protocol (experimental)
----------------------------------------

.. warning::

   **The V2 API is experimental. It may change incompatibly or be
   removed in any release.** ``hipObjGetV2()`` and ``hipObjPutV2()``
   are not implemented yet and return ``hipObjNotSupported``.

hipobj-rc-v2 runs each transfer as a session on a dedicated control
endpoint, through the ``hipObjOpsV2_t`` callbacks, each of which makes
one HTTP round trip:

#. **PREPARE** (``sendPrepare``) exchanges the client's and the
   server's RDMA tokens and starting packet sequence numbers, and the
   server opens a session.
#. The client connects its queue pair to the server's.
#. **READY** (``sendReady``) starts the transfer. The server answers
   when the transfer is done, with the **FINAL** response, which
   reports the bytes transferred and the object's ETag, version ID, and
   checksum.
#. **CANCEL** (``sendCancel``) abandons a transfer that was prepared but
   not started.

Unlike ``hipObjGet()`` and ``hipObjPut()``, the V2 functions name the
object (bucket, key, and an optional canonical query string), and their
``offset`` argument is a byte offset into the object.

What exists today
~~~~~~~~~~~~~~~~~

* ``hipObjGetV2()`` and ``hipObjPutV2()`` check their arguments, then
  return ``hipObjNotSupported``. That's also the error that means a
  server doesn't support hipobj-rc-v2, so an application that falls back
  to ``hipObjGet()`` and ``hipObjPut()`` on that error works either way.
* There's no way to give the library the control endpoint yet.
  ``hipObjInitV2()`` isn't available, so no function accepts a
  ``hipObjConfigV2_t`` or a ``hipObjControlEndpointV2_t``.
* The ``v1`` member of ``hipObjOpsV2_t`` isn't used by the V2
  functions.
* The V2 API, along with the ``hipObjNotSupported`` and ``hipObjBusy``
  error codes, only exists when ``HIPOBJECT_V2_API`` is defined. The
  ``HIPOBJECT_V2_API`` CMake option, which is on by default, defines it.
