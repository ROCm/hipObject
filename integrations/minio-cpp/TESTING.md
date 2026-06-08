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

Confirm:

- `hipObjInit` opens an RC QP (state INIT; see QP note below)
- `hipObjBufRegister` succeeds (dmabuf or host-staged fallback)
- Minted token length is 88 hex characters

### QP state (INIT vs RTR/RTS)

`hipObjInit` brings the client QP to **INIT**. The server-side RC adapter
and cuObjServer are expected to complete the RC handshake for data-plane
transfers. If end-to-end PUT/GET fail with RDMA errors while the control
plane succeeds:

1. Capture `ibv_devinfo` and server logs from cuObjServer / RC adapter.
2. Try completing client-side `transitionQpToRtr` / `transitionQpToRts` in
   `hipObjInit` once server routing parameters are known.
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
