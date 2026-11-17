hipobj-rc-v2: client and server flows
=====================================

The connection-oriented v2 transfer path (``x-amz-rdma-protocol:
hipobj-rc-v2``), as ``src/rdma/{v2-client,v2-transport,v2-registry,
v2-state,v2-wire}`` implement it on the client side and the MinIO
bridge (``integrations/minio-cpp/src/rdma.cpp``) drives end to end
against the S3 side. Unlike v1 (a single RDMA-READ token on the data
request) and v3 (a passive RDMA-CM listener), v2 establishes a
bidirectional RC connection through a small three-request control
protocol on dedicated endpoints, then moves the bytes over that
connection.

Roles
-----

- **hipObject client** owns the data buffer, registers it, and drives
  the protocol: it sends PREPARE, transitions its QP, and sends READY.
  It is also the RDMA initiator for PUT and the responder for GET.
- **S3 gateway / storage server** owns a control endpoint under
  ``/.hipobj-rc/``; it answers PREPARE with its own QP identity and a
  session id, performs the RDMA side of the transfer while answering
  READY, and echoes the outcome in that response.

Transfer sequence (PUT and GET share it)
----------------------------------------

.. code-block:: text

    Application        hipObject v2 client                S3 gateway control endpoint
         |                      |                                     |
         |--hipObjBufRegister-->|   ibv_reg_mr on the v2 device's PD |
         |                      |                                     |
         |--hipObjPut/GetV2---->|                                     |
         |                      | QP + CQ created (registry entry,    |
         |                      | retired-ring reservation taken)    |
         |                      |                                     |
         |                      |--PREPARE /.hipobj-rc/prepare------->|
         |                      |  x-amz-rdma-protocol: hipobj-rc-v2  |
         |                      |  x-amz-rdma-token: client QP        |
         |                      |    (88-hex payload: QPN, GID, RC,    |
         |                      |     port)                           |
         |                      |  x-amz-rdma-psn: client PSN         |
         |                      |  x-amz-rdma-cookie, -op, -size,     |
         |                      |    -offset, -target                 |
         |                      |<--200 + server token, session (32   |
         |                      |   hex), server PSN------------------|
         |                      |  PUT only: staging addr + rkey      |
         |                      |                                     |
         |                      | QP -> RTR (server QPN/GID/PSN),     |
         |                      | QP -> RTS (client PSN), bounded by  |
         |                      | the connect deadline               |
         |                      |                                     |
         |                      |  GET: post the receive (client MR)  |
         |                      |   BEFORE READY -- READY arms the    |
         |                      |   server's WRITE_WITH_IMM, so a     |
         |                      |   late post burns RNR retries       |
         |                      |                                     |
         |                      |--READY /.hipobj-rc/ready----------->|
         |                      |  session, cookie, PSN, client QPN,  |
         |                      |  MR addr + rkey; response pending   |
         |                      |                                     |
         |   PUT: bounded grace, then WRITE_WITH_IMM(cookie) into the|
         |   server's staging MR; poll local send completion         |
         |   GET: poll WRITE_WITH_IMM completion; imm_data = cookie, |
         |   byte_len = size                                       |
         |                      |                                     |
         |                      |<--FINAL (the READY response)--------|
         |                      |  x-amz-rdma-reply, -bytes-          |
         |                      |   transferred, echoed cookie, etag, |
         |                      |   version id, CRC64NVME checksum    |
         |                      |                                     |
         |<--hipObjSuccess-------|  connection released to the registry|

The whole transfer runs under one deadline budget: the entry timestamp
is taken before the API lock wait so the wait counts, and every phase
checks the remaining budget. A budget that runs out maps to
``hipObjBusy`` with the ``"TIME"`` diagnostic marker
(``kDiagDeadlineExpired``) so callers can tell expiry from
server-side backpressure. After the session was published, expiry
paths send one bounded CANCEL to ``/.hipobj-rc/cancel``.

Connection lifecycle and the retired ring
-----------------------------------------

The registry (``v2-registry.h``) owns every QP/CQ pair behind an
entry, all under the library-wide API lock:

.. code-block:: text

    Live --claimDestroy--> Destroying --commitDestroy(remains)--> Poisoned
    Poisoned --claimDestroy(retry)--> Destroying --commitDestroy(all gone)
         --> Destroyed --eraseDestroyed--> (removed)

On release, the entry records its (QPN, PSN) in a retired ring (4096
slots, 60 s expiry). A peer may still be sending on an old QP number;
a later reservation that lands on the same (QPN, PSN) pair takes the
stale-peer path and retires the freshly created QP instead of
answering it. An entry holds at most one buffer pin, released once
when the entry is fully reclaimed; a destroy that fails mid-way
poisons the entry so ``hipObjShutdown()`` can report the leftover.

Init generations keep the interface choice coherent: the client
captures the NIC, port, and GID index together with the generation
they were selected in (``InterfaceSnapshot``), and a transfer whose
generation no longer matches init fails fast rather than binding a
control connection on one interface and a data QP on another.

The bridge bounds detached DNS resolvers (at most four outstanding)
and caches transfer credentials, so a v2 transfer never blocks the
MinIO event loop on name resolution.

Wire format
-----------

Three requests on dedicated endpoints under ``/.hipobj-rc/``:
``prepare``, ``ready``, ``cancel``. Every request carries the
protocol header (``x-amz-rdma-protocol: hipobj-rc-v2``); the server
echoes it in its responses, and the client rejects a reply that does
not echo it.

.. list-table::
   :header-rows: 1

   * - Header
     - Direction
     - Meaning
   * - ``x-amz-rdma-Token``
     - both
     - RDMA token: 88 hex chars (QPN, GID, transport, port)
   * - ``x-amz-rdma-Session``
     - server -> client (PREPARE), client -> server (READY)
     - 128-bit session id, 32 hex chars
   * - ``x-amz-rdma-Cookie``
     - client -> server (32-bit, echoed in FINAL)
     - correlates the RDMA immediate data with the session
   * - ``x-amz-rdma-Psn``
     - both
     - starting PSN, 6 hex digits, 000001..ffffff
   * - ``x-amz-rdma-Size`` / ``-Offset``
     - client -> server
     - transfer length and object offset; length capped at 2^31-1
   * - ``x-amz-rdma-Target``
     - client -> server
     - canonical bucket/key/query path, RFC 3986 encoded
   * - ``x-amz-rdma-Bytes-Transferred``
     - server -> client
     - bytes moved, must equal the requested size
   * - ``x-amz-rdma-Checksum``
     - server -> client
     - ``CRC64NVME `` + 12 canonical base64 chars
   * - ``x-amz-rdma-Etag`` / ``-Version-Id``
     - server -> client
     - object metadata for the completed write or read

The PREPARE response doubles as the connection parameters exchange;
the READY response is the FINAL outcome, so a successful transfer is
exactly two round trips plus the RDMA data phase. An unsupported
server answers PREPARE with 501 and the
``x-amz-rdma-Protocol-Status: unsupported`` marker, which the client
maps to ``hipObjNotSupported``. A 503 maps to ``hipObjBusy`` (the
server's connection capacity or staging is exhausted), and 400/403 to
``hipObjInvalidValue``.
