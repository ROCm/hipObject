# Testing hipObject MinIO RDMA Client

This document describes how to validate the `hipobj_minio` bridge against
MinIO AIStor with cuObjServer and the RC adapter on lab hardware.

## Layer 1 — Unit tests (no GPU / RDMA / S3)

```bash
cmake -B build -DHIPOBJ_MINIO_CLIENT=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R 'test-rdma-token|test-rdma-header' --output-on-failure
```

Covers token header formatting (`token:buf:size`), numeric
`x-amz-rdma-reply` parsing, and GID-to-NIC extraction.

## Layer 2 — hipObject smoke (GPU + NIC, no S3)

On an AMD GPU node with `bnxt_re` or `ionic_rdma`:

```bash
ibv_devinfo
rocminfo
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hipobj-examples/get-object 67108864
```

This invocation uses the example's stub callbacks: it does not contact
an S3 server or transfer object data. After initialization and buffer
registration succeed, the stub reply reports `501` (not implemented),
so the example ends with `hipObjGet failed: S3 error` and exit status
1. That nonzero exit is expected for this layer.

Confirm:

- stderr reaches `[get-object] would send RDMA token (88 bytes)`:
  reaching this callback proves `hipObjInit` opened the RC QP (state
  INIT; see QP note below) and `hipObjBufRegister` succeeded
  (dmabuf or host-staged fallback), and the minted token length is
  88 hex characters
- the final `hipObjGet failed: S3 error` line and exit status 1 are
  the expected stub outcome

An exit status 1 without the token line means an earlier failure, such
as initialization or buffer registration.

### QP state (INIT vs RTR/RTS)

`hipObjInit` brings the client QP to **INIT**. After a successful S3
response, `hipObjGet` / `hipObjPut` complete the RC handshake when the
server returns a peer token in `x-amz-rdma-reply` (format
`200:<server-token-hex>`). The in-repo `hipobj-rdma-test-server` uses
this format; MinIO AIStor + RC adapter may return numeric codes only.

If end-to-end PUT/GET fail with RDMA errors while the control plane
succeeds:

1. Capture `ibv_devinfo` and server logs from cuObjServer / RC adapter.
2. Confirm whether the server reply includes a peer token for RC connect.
3. Verify vendor QP attributes: BNXT/IONIC backends apply `configureBnxtQp`
   / `configureIonicQp` during RTR/RTS transitions in
   [`src/rdma/transport.cpp`](../../src/rdma/transport.cpp).

## Layer 3 — AIStor end-to-end

### Server (AIStor node)

- MinIO AIStor with S3-over-RDMA enabled
- `cuObjServer` running
- RC adapter bridging AMD RC clients to DC cuObjServer
- RoCEv2 fabric with PFC/ECN; `rdma link` / `show_gids` show reachable GIDs

### Client (AMD GPU node)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHIPOBJ_MINIO_CLIENT=ON \
  -DHIPOBJ_BNXT=ON
cmake --build build
```

Install runtime deps: ROCm, `libibverbs`, `libnuma`, OpenSSL, libcurl.

### Procedure

1. **Control-plane probe**

   ```bash
   mc alias set lab https://<aistor-host> <access-key> <secret-key>
   mc mb lab/hipobj-test
   ```

2. **RDMA PUT + GET**

   ```bash
   ./build/integrations/minio-cpp/minio-getput-rdma \
     <host> <access-key> <secret-key> 10485760 gpu
   ```

   Expect `PUT ok` with an etag and `Data integrity check passed`.

3. **HTTP fallback (501)**

   Disable RDMA on the server or use a non-RDMA bucket. The client should
   complete via HTTP without crashing (`x-amz-rdma-reply: 501`).

4. **Size sweep**

   Run with `4096`, `1048576`, `67108864`, and `1073741824` byte sizes.

5. **Negative cases**

   - Unregistered buffer → `hipObjBufNotRegistered`
   - Wrong credentials → S3 error before RDMA
   - NIC failure → retry (`kRdmaMaxAttempts = 2`) then HTTP fallback

### Optional interoperability

PUT with NVIDIA `GetPutRDMA` (minio-cpp + cuObj), GET with
`minio-getput-rdma` on AMD, to confirm wire compatibility through the RC
adapter.

## CI limitations

GitHub Actions ROCm containers have no RDMA NIC or AIStor endpoint. Layer 1
runs in CI; Layers 2–3 require lab hardware or a self-hosted runner.
