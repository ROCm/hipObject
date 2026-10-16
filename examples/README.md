# hipObject examples

These programs show how to use the hipObject API to move an S3 object
between an RDMA-capable S3 server and GPU memory.

| Program | What it does |
| --- | --- |
| `get-object` | Downloads an object into GPU memory (`hipObjGet()`) |
| `put-object` | Uploads GPU memory to an object (`hipObjPut()`) |

`s3_curl_ops.cpp` and `s3_curl_ops.h` are the S3 side: the callbacks that
put hipObject's RDMA token in an S3 request with libcurl and read the
server's reply.

## Building

hipObject installs these files in `share/doc/hipobject<mm>/examples` under
its install prefix, such as
`/opt/rocm/core-10.1/share/doc/hipobject10.1/examples`. They're in the
development package (`hipobject-dev<mm>`, or `hipobject-devel<mm>` for
RPM), which also has the header and CMake package they build against.

Copy the directory somewhere writable, then configure and build it:

```bash
cp -r /opt/rocm/core-10.1/share/doc/hipobject10.1/examples hipobject-examples
cd hipobject-examples
cmake -B build -DCMAKE_PREFIX_PATH=/opt/rocm/core-10.1
cmake --build build
```

Set `CMAKE_PREFIX_PATH` to where ROCm and hipObject are installed. The
examples are written to C++11 and build with any C++11 compiler.

The `--live` mode needs libcurl's development files (`libcurl4-openssl-dev`
on Ubuntu, `libcurl-devel` on RHEL). Without them, the examples build
with only the stub mode.

## Running

The examples need an AMD GPU and an RDMA NIC. Set `HIPOBJ_NIC_HINT` to the
NIC's RDMA device name (as `ibv_devices` lists it) to choose the NIC
yourself.

```bash
build/get-object <size-bytes> [--live URL [bucket] [object]]
build/put-object <size-bytes> [--live URL [bucket] [object]]
```

- **Without `--live`**, the callbacks are stubs that contact no server.
  The program initializes hipObject, registers a GPU buffer, and prints
  `would send RDMA token` when hipObject hands the callback a token. The
  stub then replies `501` (not implemented), so the program ends with
  `hipObjGet failed: S3 error` (or `hipObjPut failed`) and exits with
  status 1. Reaching the `would send RDMA token` line means everything up
  to the transfer worked.
- **With `--live URL`**, the program sends the request to an S3 server
  that supports RDMA transfers (the `x-amz-rdma-token` header) at `URL`,
  such as `http://127.0.0.1:9000`. The bucket defaults to `test` and the
  object to `object`. The requests aren't signed, so the bucket has to
  allow anonymous access. The program prints `hipObjGet succeeded` (or
  `hipObjPut succeeded`) when the transfer works.
