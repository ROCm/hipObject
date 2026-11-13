hipobj-rc-v3: client and server flows
=====================================

The RC-CM-based RDMA path (``x-amz-rc-rdma-token`` and
``x-amz-rc-rdma-reply``), as ``src/hipobj.cpp`` and
``src/rdma/everpure/{rc-listener,v3-wire}.*`` implement it, and as
``test/integration/aws-s3-client/`` exercises it end to end. hipObject
uses it when it's built with ``-DHIPOBJ_RDMA_WIRE_V3=ON``; the default
build speaks v1 (``x-amz-rdma-token``).

The roles are inverted relative to a "normal" client/server picture:

- **hipObject client** (the application host, such as
  ``hipobj-aws-s3-demo``) runs an RC-CM **listener** and holds the actual
  data buffer. It never performs RDMA itself -- it just tells the server
  where to dial and what memory to touch.
- **S3 gateway / storage server** is the RDMA **initiator**: it dials the
  client's listener and performs the one-sided RDMA READ (PUT) or WRITE
  (GET).

PUT (upload): server does an RDMA READ from the client's buffer
---------------------------------------------------------------

.. code-block:: text

    Application          hipObject client               RC-CM listener             S3 gateway
         |                      |                              |                          |
         |--hipObjInit(config)->|                              |                          |
         |                      |--RdmaCmRcListener::Start---->|                          |
         |                      |     (bindIp: HIPOBJ_RC_LISTEN_IP or the chosen NIC's    |
         |                      |      IPv4; its own PD on the context the bind resolves  |
         |                      |      to)                     | background accept-loop   |
         |                      |                              | thread starts            |
         |                      |                              |                          |
         |--hipObjBufRegister(devPtr, size)-------------------->|                          |
         |                      | ibv_reg_mr(listener's pd, devPtr, size,                  |
         |                      |   REMOTE_READ|REMOTE_WRITE|LOCAL_WRITE) -> rkey          |
         |                      |                              |                          |
         |--hipObjPut(devPtr, size, ops)------------------------|                          |
         |                      | buildRdmaTokenV3(): listenIp, listenPort, rkey,          |
         |                      |   remoteAddr, length -> encodeRdmaTokenV3Hex()           |
         |                      |                              |                          |
         |                      |========== x-amz-rc-rdma-token (HTTP request header) =====|
         |                      |  "<ip-hex32>:<port-hex4>:<rkey-hex8>:<remoteAddr-hex16>:<length-hex16>"
         |                      |----HTTP PUT /bucket/key, empty body-------------------->|
         |                      |                              |                          |
         |                      |                              |    parse_rc_rdma_token():
         |                      |                              |    rem_desc_.rkey/raddr/rsize = fields
         |                      |                              |                          |
         |                      |========== RDMA CM: rdma_resolve_addr/route -> rdma_connect() ====
         |                      |                              |     dials listenIp:listenPort from the token
         |                      |                              |<--RDMA_CM_EVENT_CONNECT_REQUEST-|
         |                      |                              | ibv_create_cq() +        |
         |                      |                              |  rdma_create_qp(id, pd)  |
         |                      |                              |----rdma_accept()-------->|
         |                      |                              |   RDMA CM negotiates INIT/RTR/RTS
         |                      |                              |   itself -- no QP#/GID/LID ever
         |                      |                              |   goes over HTTP         |
         |                      |                              | RDMA_CM_EVENT_ESTABLISHED|
         |                      |                              |  (establishedCount++)    |
         |                      |                              |                          |
         |                      |========== one-sided RDMA READ (RC transport, RoCEv2) ============
         |                      |    server pulls the object bytes directly out of the client's
         |                      |    registered memory -- no CPU copy, no further app messages
         |                      |<---------RDMA READ Request (rkey, remoteAddr, len=size)---------|
         |                      |----------RDMA READ Response: payload = devPtr[0..size)--------->|
         |                      |                              |     RC completion: status 0
         |                      |                              |     write object bytes to
         |                      |                              |     backing store         |
         |                      |                              |                          |
         |                      |========== x-amz-rc-rdma-reply (HTTP response header) ===========
         |                      |  "<httpStatus>:<bytesTransferred-hex16>"                 |
         |                      |<---HTTP 200 OK  x-amz-rc-rdma-reply: 200:0000000000001000-------|
         |                      |    ETag: "..."               |                          |
         |                      | receiveRdmaReplyV3() -> decodeRdmaReplyV3():             |
         |                      |   httpStatus==200 and        |                          |
         |                      |   bytesTransferred==size -> success                      |
         |<--hipObjPut() = hipObjSuccess--|                     |                          |
         |                      |                              |                          |
         |--hipObjBufDeregister(devPtr)------------------------>|                          |
         |                      | ibv_dereg_mr(mr)             |                          |

GET (download): server does an RDMA WRITE into the client's buffer
------------------------------------------------------------------

Symmetric to PUT, with the transfer direction reversed. The client still
owns the listener and the buffer; the server still initiates the RDMA
operation.

.. code-block:: text

    Application          hipObject client               RC-CM listener             S3 gateway
         |                      |                              |                          |
         |--hipObjBufRegister(devPtr, size)-------------------->|                          |
         |                      | ibv_reg_mr(...) -> rkey       |                          |
         |                      |                              |                          |
         |--hipObjGet(devPtr, size, ops)-------------------------|                         |
         |                      | buildRdmaTokenV3() + encodeRdmaTokenV3Hex()              |
         |                      |                              |                          |
         |                      |========== x-amz-rc-rdma-token ============================
         |                      |----HTTP GET /bucket/key-------------------------------->|
         |                      |    x-amz-rc-rdma-token: 00000000000000000000ffff0a000005:4705:0007cb8a:0000...  |
         |                      |                              |    parse_rc_rdma_token():
         |                      |                              |    read object bytes from
         |                      |                              |    backing store          |
         |                      |                              |                          |
         |                      |========== RDMA CM connect (same as PUT) ==================
         |                      |                              |<--CONNECT_REQUEST--------|
         |                      |                              |----accept--------------->|
         |                      |                              |  ESTABLISHED             |
         |                      |                              |                          |
         |                      |========== one-sided RDMA WRITE ===========================
         |                      |    server pushes the object bytes directly into the      |
         |                      |    client's registered memory                            |
         |                      |<---------RDMA WRITE (rkey, remoteAddr) -- payload = object bytes--|
         |                      |                              |     RC completion: status 0
         |                      |                              |                          |
         |                      |========== x-amz-rc-rdma-reply ============================
         |                      |<---HTTP 200 OK  x-amz-rc-rdma-reply: 200:0000000000001000--------|
         |                      | decodeRdmaReplyV3(): httpStatus==200,                    |
         |                      |   bytesTransferred==size -> success                      |
         |<--hipObjGet() = hipObjSuccess (devPtr now holds the object)--|                  |
         |                      |                              |                          |
         |--hipObjBufDeregister(devPtr)------------------------>|                          |

RC-CM listener lifecycle
------------------------

How the pieces from the two flows above actually come up and go down, and
which thread each step runs on. The listener calls librdmacm and
libibverbs through the platform's ``RdmaCm`` and ``Verbs`` (``Platform``
in ``state.h``), so the unit tests run it on fakes.

.. code-block:: text

    Main thread (App/hipObj* calls)   Accept-loop thread (rc-listener.cpp)   RDMA CM / libibverbs
         |                                    |                                    |
         |  RdmaCmRcListener::Start(bindIp)   |                                    |
         |  bindIp: HIPOBJ_RC_LISTEN_IP, or   |                                    |
         |  the chosen NIC's IPv4 GID. No     |                                    |
         |  address, a failed start, or a     |                                    |
         |  bind on another NIC: hipObjInit() |                                    |
         |  fails with hipObjRdmaError. The   |                                    |
         |  port is HIPOBJ_RC_LISTEN_PORT's.  |                                    |
         |--rdma_bind_addr(bindIp)------------------------------------------------>|
         |    RDMA CM picks the device        |                                    |
         |--ibv_alloc_pd(listenId->verbs)----------------------------------------->|
         |    buffers register on this PD     |                                    |
         |--start the accept-loop thread----->|                                    |
         |                                    |                                    |
         |          .-------- per accepted connection ------------------------.   |
         |          |                         |<--RDMA_CM_EVENT_CONNECT_REQUEST----|
         |          |                         |--ibv_create_cq +------------------>|
         |          |                         |   rdma_create_qp(pd)               |
         |          |                         |--rdma_accept()--------------------->|
         |          |                         |<--RDMA_CM_EVENT_ESTABLISHED---------|
         |          |   Other threads may register or deregister buffers on the   |
         |          |   same PD at the same time; libibverbs is thread-safe       |
         |          |                         |<--RDMA_CM_EVENT_DISCONNECTED--------|
         |          |                         |--rdma_destroy_qp +---------------->|
         |          |                         |   ibv_destroy_cq                   |
         |          |                         |--rdma_destroy_id(id)-------------->|
         |          '-----------------------------------------------------------'   |
         |                                    |                                    |
         |--hipObjShutdown(): when the last call holding the instance returns,  |
         |    its members go in reverse order: the buffers first (their MRs    |
         |    are on the listener's PD, which can't be deallocated with MRs   |
         |    left on it), then the listener  |                                    |
         |--Stop()--------------------------->|                                    |
         |                                    | stopping_ = true; join thread      |
         |--ibv_dealloc_pd(pd_)------------------------------------------------->|
         |    (only after the thread is joined -- no concurrent accept-loop      |
         |     activity left)                 |                                    |

Wire formats (hipobj-rc-v3, ``src/rdma/everpure/v3-wire.h``)
------------------------------------------------------------

.. list-table::
   :header-rows: 1

   * - Header
     - Direction
     - Format
   * - ``x-amz-rc-rdma-token``
     - client -> server
     - ``<ip-hex32>:<port-hex4>:<rkey-hex8>:<remoteAddr-hex16>:<length-hex16>``
   * - ``x-amz-rc-rdma-reply``
     - server -> client
     - ``<httpStatus>:<bytesTransferred-hex16>``

Both are single ASCII lines, safe as literal HTTP header values. No
protocol version tag -- this is the only wire format in play for v3.
``httpStatus`` of ``200``/``204``/``206`` is success; anything else is
failure. Both messages share the same field delimiter, ``kFieldDelim``
(``:``). ``ip`` is the listener's address as 16 raw bytes, hex-encoded:
IPv4 addresses use the IPv4-mapped IPv6 form (RFC 4291 2.5.5.2 --
``00000000000000000000ffff<4 IPv4 bytes>``). ``port`` is hex too (4
digits), matching ``rkey``/``remoteAddr``/``length``; ``httpStatus`` in
the reply stays decimal since it's an HTTP status code.

The default v1 wire, cuObject's ``x-amz-rdma-token`` and
``x-amz-rdma-reply``, works differently (the token carries a QP number
and GID instead of a listener's address), and this page doesn't cover
it.
