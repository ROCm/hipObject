# hipobj-rc-v3: client/server flow diagrams

Scope: the RC-CM-based RDMA path (`x-amz-rc-rdma-token` / `x-amz-rc-rdma-reply`),
as implemented in `src/hipobj.cpp`, `src/rdma/everpure/{rc-listener,v3-wire}.*`,
and exercised end-to-end by `test/integration/aws-s3-client/`.

Roles, which invert relative to a "normal" client/server picture:

- **hipObject client** (the application host, e.g. this demo) runs an RC-CM
  **listener** and holds the actual data buffer. It never performs RDMA
  itself -- it just tells the server where to dial and what memory to
  touch.
- **S3 gateway / storage server** is the RDMA **initiator**: it dials the client's listener and
  performs the one-sided RDMA READ (PUT) or WRITE (GET).

## PUT (upload): server does an RDMA READ from the client's buffer

```
 Application          hipObject client               RC-CM listener             S3 gateway
      |                      |                              |                          |
      |--hipObjInit(config)->|                              |                          |
      |                      |--startRcListener(bindIp)---->|                          |
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
      |                      |                              |  (PdLockGuard held)      |
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
      |<--hipObjPut() = HIPOBJ_SUCCESS--|                    |                          |
      |                      |                              |                          |
      |--hipObjBufDeregister(devPtr)------------------------>|                          |
      |                      | ibv_dereg_mr(mr) (PdLockGuard held)                      |
```

## GET (download): server does an RDMA WRITE into the client's buffer

Symmetric to PUT, with the transfer direction reversed. The client still
owns the listener and the buffer; the server still initiates the RDMA
operation.

```
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
      |<--hipObjGet() = HIPOBJ_SUCCESS (devPtr now holds the object)--|                 |
      |                      |                              |                          |
      |--hipObjBufDeregister(devPtr)------------------------>|                          |
```

## RC-CM listener lifecycle

How the pieces from the two flows above actually come up and go down, and
which thread each step runs on -- relevant to the cross-thread PD-safety
fix (`PdLockGuard`, see `rc-listener.h`).

```
 Main thread (App/hipObj* calls)   Accept-loop thread (rc-listener.cpp)   RDMA CM / libibverbs
      |                                    |                                    |
      |--startRcListener(bindIp, port=0)-->|                                    |
      |  bindIp: HIPOBJ_RC_LISTEN_IP, or   |                                    |
      |  the chosen NIC's IPv4 GID. No     |                                    |
      |  address, a failed start, or a     |                                    |
      |  bind on another NIC: hipObjInit() |                                    |
      |  fails with hipObjRdmaError        |                                    |
      |                                    |--rdma_bind_addr(bindIp)----------->|
      |                                    |    RDMA CM picks the device        |
      |                                    |--ibv_alloc_pd(listenId->verbs)---->|
      |                                    |    buffers register on this PD     |
      |                                    | spawn background thread(acceptLoop)|
      |                                    |                                    |
      |          .-------- per accepted connection ------------------------.   |
      |          |                         |<--RDMA_CM_EVENT_CONNECT_REQUEST----|
      |          |                         |--[PdLockGuard] ibv_create_cq +---->|
      |          |                         |   rdma_create_qp(pd)               |
      |          |                         |--rdma_accept()--------------------->|
      |          |                         |<--RDMA_CM_EVENT_ESTABLISHED---------|
      |          |   Main thread may concurrently be inside hipObjBufRegister/  |
      |          |   Deregister on the SAME pd -- PdLockGuard serializes both   |
      |          |                         |<--RDMA_CM_EVENT_DISCONNECTED--------|
      |          |                         |--[PdLockGuard] rdma_destroy_qp +-->|
      |          |                         |   ibv_destroy_cq                   |
      |          |                         |--rdma_destroy_id(id)-------------->|
      |          '-----------------------------------------------------------'   |
      |                                    |                                    |
      |--hipObjShutdown(): ~Instance() deregisters every buffer first--------->|
      |    (their MRs are on the listener's PD, which can't be deallocated   |
      |     with MRs left on it)           |                                    |
      |--listener.reset(): Stop()--------->|                                    |
      |                                    | stopping_ = true; join thread      |
      |--[PdLockGuard] ibv_dealloc_pd(pd_)----------------------------------->|
      |    (only after the thread is joined -- no concurrent accept-loop      |
      |     activity left)                 |                                    |
```

## Wire formats (hipobj-rc-v3, `src/rdma/everpure/v3-wire.h`)

| Header | Direction | Format |
|---|---|---|
| `x-amz-rc-rdma-token` | client -> server | `<ip-hex32>:<port-hex4>:<rkey-hex8>:<remoteAddr-hex16>:<length-hex16>` |
| `x-amz-rc-rdma-reply` | server -> client | `<httpStatus>:<bytesTransferred-hex16>` |

Both are single ASCII lines, safe as literal HTTP header values. No
protocol version tag -- this is the only wire format in play for v3.
`httpStatus` of `200`/`204`/`206` is success; anything else is failure.
Both messages share the same field delimiter, `kFieldDelim` (`:`).
`ip` is the listener's address as 16 raw bytes, hex-encoded: IPv4
addresses use the IPv4-mapped IPv6 form (RFC 4291 2.5.5.2 --
`00000000000000000000ffff<4 IPv4 bytes>`). `port` is hex too (4
digits), matching `rkey`/`remoteAddr`/`length`; `httpStatus` in the
reply stays decimal since it's an HTTP status code.
Contrast with the older v1/v2 (DC-RDMA/CuObj) path's `x-amz-rdma-token` /
`x-amz-rdma-reply` + separate `x-amz-rdma-bytes-transferred` header,
which this diagram set does not cover.
