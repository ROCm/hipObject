Architecture
============

This page describes how the hipObject library does its work: the
code in ``src/``, the helpers in ``shared/``, and the public header
``include/hipobj.h``. It doesn't cover the build system, the tests,
the examples, or the MinIO bridge in ``integrations/``. The wire
formats are in :doc:`interop`, and the rules for changing the code
are in ``AGENTS.md`` in the repository root.

.. contents:: On this page
   :local:
   :depth: 1

What the Library Does
---------------------

hipObject moves the data of an S3 object between an S3 server and a
buffer in GPU or host memory by RDMA, so that the payload doesn't
pass through the client's CPU. It does **not** speak HTTP or S3. The
work is split three ways:

- **The application** owns the S3 conversation. It sends the S3
  request, with an RDMA token from hipObject in the
  ``x-amz-rdma-token`` header, and hands the ``x-amz-rdma-reply``
  header from the response back to hipObject. It does this either
  through the two ``hipObjOps_t`` callbacks that ``hipObjGet()`` and
  ``hipObjPut()`` call, or by driving the transfer itself with
  ``hipObjGetRdmaToken()``, ``hipObjBufSync()``, and
  ``hipObjParseRdmaReply()``.
- **The S3 server** moves the payload with one-sided RDMA: an RDMA
  WRITE into the client's buffer for a GET, an RDMA READ from it for
  a PUT. The client posts no work requests of its own.
- **hipObject** prepares the client side: it chooses the NIC closest
  to the GPU, opens it, registers buffers with it (as dma-bufs for
  GPU memory), mints the tokens that describe a buffer and a queue
  pair (QP), parses the server's reply, connects the QP to the peer
  the reply names, waits for the transfer's completion, and copies
  through host memory when the NIC can't reach the GPU directly.

The headers are wire-compatible with NVIDIA cuObject, but hipObject
uses the Reliable Connection (RC) transport, not cuObject's Dynamic
Connection (DC), on Broadcom (bnxt) and AMD Pensando (ionic) NICs.

Source Map
----------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - Role
   * - ``include/hipobj.h``
     - The public C API, including the thread-safety contract (the
       ``threads`` group)
   * - ``src/hipobj.cpp``
     - The C API's implementation: argument checks, the transfer
       sequence, staging copies
   * - ``src/state.h``, ``state.cpp``
     - ``Platform``, ``Options``, ``Instance``, the current instance,
       init and shutdown
   * - ``src/buffer.h``, ``buffer.cpp``
     - ``BufferMap``: registered buffers, dma-buf and staging-buffer
       registration
   * - ``src/abandoned-copies.h``, ``.cpp``
     - ``AbandonedCopies``: staging copies that timed out but may
       still run
   * - ``src/rdma/transport.h``, ``.cpp``
     - ``RdmaDevice``, ``RcConnection``, QP state transitions,
       completion polling
   * - ``src/rdma/connection-pool.h``, ``.cpp``
     - ``ConnectionPool`` and ``Lease``: one connection per transfer
   * - ``src/rdma/token.h``, ``.cpp``
     - Token encoding and decoding, ``x-amz-rdma-reply`` parsing
   * - ``src/rdma/vendor-ops.h``, ``bnxt/``, ``ionic/``
     - Vendor QP attributes, compiled in by ``HIPOBJ_BNXT`` and
       ``HIPOBJ_IONIC``
   * - ``src/s3/control.h``, ``.cpp``
     - Calling the ``hipObjOps_t`` callbacks
   * - ``src/common/hip-seam.*``
     - ``HipRuntime`` interface and ``SystemHip`` (HIP, plus HSA's
       dma-buf export)
   * - ``src/common/ibv-wrapper.*``
     - ``Verbs`` interface and ``LoadedVerbs`` (libibverbs through
       ``dlopen()``)
   * - ``src/common/ibv-core.h``
     - hipObject's own copy of the libibverbs ABI types
   * - ``src/common/ibv-ptr.h``
     - ``std::unique_ptr`` types for verbs objects, and ``own()``
   * - ``src/common/nic-seam.h``, ``rdma-topology.*``
     - ``NicTopology`` interface, ``SysfsTopology``, NIC and GID
       selection
   * - ``src/common/thread-safety.h``
     - Mutexes and locks annotated for Clang's thread safety analysis
   * - ``src/common/no-destructor.h``, ``dl-handle.h``, ``gid.h``,
       ``ibv-gid.h``
     - Small helpers
   * - ``shared/``
     - Helpers shared with tools and tests: span, address, parse,
       sockaddr, warnings

Layers
------

.. code-block:: text

                 application, with its own S3 client
                     |                         ^
         hipobj.h C API calls          hipObjOps_t callbacks
                     v                         |
   +-----------------------------------------------------------------+
   | hipobj.cpp: argument checks, transfer sequence, staging copies  |
   +-----------------------------------------------------------------+
   | state: the current Instance                                     |
   |   options | device | tokenConn | pool | buffers | abandoned     |
   +----------------+-------------------+--------------+-------------+
   | buffer,        | rdma/transport,   | rdma/token   | s3/control  |
   | abandoned-     | connection-pool,  |              |             |
   | copies         | vendor attributes |              |             |
   +----------------+-------------------+--------------+-------------+
   | Platform:   HipRuntime    |    Verbs      |    NicTopology      |
   +---------------------------+---------------+---------------------+
                |                      |                  |
      HIP runtime, and HSA      libibverbs.so.1      /sys/class/
      for dma-buf export        (dlopen)             infiniband

Calls go down the diagram, or across a row, never up. Nothing below
``hipobj.cpp`` calls a public function, and only the three
``Platform`` implementations call the HIP runtime, libibverbs, or
sysfs.

The Instance
------------

All of the library's state lives in one ``Instance``
(``src/state.h``), built by ``hipObjInit()`` and released after
``hipObjShutdown()``. It holds:

- ``platform``: the outside world (see `The Platform`_).
- ``options``: settings read from the environment once, at init (see
  `Configuration`_).
- ``config``: the GPU and NIC that init chose. Nothing in the library
  reads it after init; the tests do.
- ``device``: the open ``RdmaDevice``: its verbs context, protection
  domain (PD), port, GID, and vendor ID.
- ``tokenConn``: the one connection whose QP number
  ``hipObjGetRdmaToken()`` puts in its tokens.
- ``pool``: the ``ConnectionPool`` that ``hipObjGet()`` and
  ``hipObjPut()`` lease connections from.
- ``buffers``: the ``BufferMap`` of registered buffers.
- ``abandoned``: the ``AbandonedCopies`` list.

Everything but the last three is fixed once the instance is built,
and those three each have a lock of their own, so the instance needs
no lock.

Finding the Instance
^^^^^^^^^^^^^^^^^^^^

The process-wide state is a ``Current`` (``src/state.cpp``): the
instance in a ``std::shared_ptr``, a shared mutex that guards it,
and an ``initMutex`` that init and shutdown hold. It's held in a
``NoDestructor``, so it's never destroyed: destroying an instance
releases verbs and HIP resources, which isn't safe during static
destruction.

Every public function that needs the library starts with
``currentInstance()``, which copies the ``shared_ptr`` under a
shared lock and releases the lock. The copy keeps the instance alive
for the rest of the call, even if another thread shuts the library
down meanwhile, and the instance found is passed down to everything
the call does. This is why nothing inside the library calls a public
function: it could find a different instance.

Init
^^^^

``hipObjInit()`` checks its ``hipObjConfig_t``, returns
``hipObjAlreadyInitialized`` if there's an instance, gets the system
platform (see `The Platform`_), reads the options (see
`Configuration`_), and calls ``initWith()``, which holds
``initMutex`` throughout:

#. Resolves the GPU. A ``gpuDevice`` of -1 means the calling
   thread's current HIP device. Having no GPU at all is only
   acceptable with a ``nicHint``.
#. Chooses the NIC (see `NIC and GID Selection`_).
#. Opens the device, which also chooses the port and GID.
#. Creates the token connection.
#. Builds the ``Instance``, whose ``ConnectionPool`` creates
   ``HIPOBJ_MAX_TRANSFERS`` connections. Creating fewer is a
   failure.
#. Publishes the instance under the exclusive lock.

Everything is built in locals first, so a failure at any step
releases what was built, and nothing is published.

``initWith()`` takes the ``Platform`` and ``Options`` as parameters;
the tests call it directly with a fake platform.

Shutdown
^^^^^^^^

``hipObjShutdown()`` (``shutdownInstance()``) moves the instance out
of ``Current``, so later calls find the library uninitialized, and
closes its connection pool, which wakes any transfer waiting for a
connection with ``hipObjNotInitialized``. It doesn't wait for calls
in progress: the instance is destroyed when the last call holding it
returns. A new ``hipObjInit()`` can run before then, and the old
calls keep using the old instance.

The instance's members are destroyed in reverse order, which gives
the teardown order the verbs objects need:

#. ``abandoned``: copies that are over are released; the staging
   buffers of those still running are leaked on purpose.
#. ``buffers``: each MR is deregistered, then its staging buffer
   freed.
#. ``pool``, then ``tokenConn``: each QP is destroyed, then its CQ.
#. ``device``: the PD is deallocated, then the context closed.
#. ``platform`` outlives all of them.

The Platform
------------

``Platform`` (``src/state.h``) bundles references to three
interfaces, which are the library's only route to the outside world:

.. list-table::
   :header-rows: 1
   :widths: 18 18 64

   * - Interface
     - Production class
     - Reaches
   * - ``HipRuntime``
     - ``SystemHip``
     - HIP: devices, PCI bus ids, pinned host memory, async copies,
       events, pointer attributes; HSA's
       ``hsa_amd_portable_export_dmabuf()``
   * - ``Verbs``
     - ``LoadedVerbs``
     - libibverbs: devices, PDs, MRs (plain and dma-buf), CQs, QPs,
       polling
   * - ``NicTopology``
     - ``SysfsTopology``
     - ``/sys/class/infiniband``: each NIC's PCI bus id and NUMA
       node, and the RoCE version of each GID

Their methods are ``const``, so one object serves every thread.
Objects below the instance hold a reference to the narrowest
interface they need, given to their constructors; there are no
global accessors. The tests substitute a ``FakePlatform`` and run
the rest of the library unchanged.

``systemPlatform()`` builds the production platform on first use and
never destroys it, so an application that never calls
``hipObjInit()`` never loads libibverbs, and once loaded, libibverbs
stays loaded.

libibverbs
^^^^^^^^^^

The library doesn't link to libibverbs or include its headers.
``loadLibibverbs()`` opens ``libibverbs.so.1`` with ``dlopen()``, by
its soname, which the runtime package installs, and looks up each
function with ``dlsym()``. ``ibv-core.h`` declares the ABI's
structures itself (derived from rocSHMEM by way of rocm-xio), which
ties hipObject to that soname's ABI. Two parts are optional:

- ``ibv_reg_dmabuf_mr()`` (rdma-core 34 and later). Without it,
  ``supportsDmabuf()`` is false, and GPU memory can only be
  registered with a plain ``ibv_reg_mr()`` (see
  `Buffer Registration`_).
- ``ibv_poll_cq()`` and ``ibv_post_send()``, which are inline
  functions in the real header. Without the symbols, ``LoadedVerbs``
  calls the provider's entries in the context's ops table, as the
  inline functions do.

Every verbs object is owned by a ``std::unique_ptr`` whose deleter
holds the ``Verbs`` it came from (``ibv-ptr.h``), created with
``own<Ptr>(verbs, raw)``. ``HostBufPtr`` (pinned host memory) and
``HipEventPtr`` do the same with the ``HipRuntime``.
``dropOwnership()`` gives up an object without releasing it, for the
deliberate leaks described below.

NIC and GID Selection
---------------------

At init (``src/common/rdma-topology.cpp``):

NIC
   ``GetClosestNicToGpu()`` asks HIP for the GPU's PCI bus id and the
   topology for every NIC with an active port. ``nicHint`` here is a
   comma-separated allow-list of device names. The distance between
   two bus ids is the number of characters after their common prefix,
   ignoring the PCI function; the first of the closest NICs wins. It's
   a heuristic over the bus-id strings, not a walk of the PCIe tree.
   The topology also reports each NIC's NUMA node, but the selection
   doesn't use it.

Fallback
   If there's no GPU, or no NIC with a known bus id, and ``nicHint``
   is set, the hint is opened as a device name directly. This is how
   GPU-less hosts, such as the emulated-NIC CI lanes, initialize.
   Without a hint, init fails with ``hipObjNicNotFound``.

Port
   The device is always opened on port 1, although the topology
   records each NIC's first *active* port.

GID
   ``PickBestGid()`` reads the port's GID table, skips unconfigured
   entries and those whose RoCE version is unknown, and prefers, from
   best to worst: RoCE v2 IPv4-mapped, RoCE v2 global, RoCE v2
   link-local, RoCE v1 global, RoCE v1 link-local. Ties go to the
   lowest index; with no candidate, index 0 is used.

Vendor
   The device's PCI vendor ID is read once, so that connecting a QP
   can apply the bnxt or ionic attributes without querying it.

Connections
-----------

``RcConnection`` (``src/rdma/transport.h``) is an RC QP and the CQ it
completes on, both owned, on an ``RdmaDevice`` that has to outlive
it. ``createRcConnection()`` creates a 16-entry CQ and a QP with 8
send and 8 receive work requests of one SGE each, and moves the QP to
INIT with remote read and write access.

QP state changes:

- ``connectRcPeer()`` moves INIT to RTR, aimed at the peer token's QP
  number and GID with a 4096-byte MTU and the device's GID index,
  then to RTS (timeout 14, 7 retries, 7 RNR retries, PSNs 0). If the
  device is a Broadcom (``0x14E4``) or Pensando (``0x1DD8``) NIC and
  that backend was compiled in, its attributes are applied; today
  both set the same values the generic code does.
- ``resetRcConnection()`` moves the QP to RESET, drains the CQ, and
  moves it back to INIT, ready for a different peer. An RC QP can't
  reconnect from RTS any other way.
- ``pollCompletion()`` polls the CQ, sleeping 1 ms between empty
  polls, until a completion arrives, a completion reports an error,
  or the timeout passes. An error completion's status and vendor
  error are printed to stderr, since nothing else keeps them.

The Connection Pool
^^^^^^^^^^^^^^^^^^^

Each ``hipObjGet()`` and ``hipObjPut()`` needs a QP of its own, since
a connected RC QP is bound to one peer. ``ConnectionPool``
(``src/rdma/connection-pool.h``) creates a fixed set of connections
at init and leases them out:

- ``acquire()`` returns a ``Lease``, an RAII handle to one
  connection. If none is free it waits on a condition variable,
  unless the calling thread already holds a lease (a thread-local
  count): then it returns ``Busy``, since the connection it would
  wait for could be its own, as when a transfer's callback starts
  another transfer.
- When a lease ends, a connection that was connected is reset. If it
  can't be reset it's replaced with a new one, and if that fails too
  it's dropped and the pool shrinks; an empty pool fails transfers
  with ``hipObjRdmaError``.
- ``close()``, from shutdown, makes ``acquire()`` fail with
  ``Closed``, including calls already waiting.

The connection slots are a vector whose size never changes, so a
lease can use its slot without the pool's lock; the lock only guards
the free list and counters.

The Token Connection
^^^^^^^^^^^^^^^^^^^^

``hipObjGetRdmaToken()`` serves transfers the application drives
itself. hipObject can't tell when such a transfer ends, so it can't
lease a connection for it. Instead, every such token names the
instance's single ``tokenConn``, which is never leased, never
connected, and stays in INIT. This is why the API documentation says
that application-driven transfers mustn't overlap each other.

Buffer Registration
-------------------

``BufferMap`` (``src/buffer.h``) is a ``std::map`` from the address a
buffer was registered at to a ``std::shared_ptr<const BufEntry>``.
Lookups are by that exact address: a transfer passes the registered
address and selects part of the buffer with its offset. The map holds
at most 256 buffers, each up to 4 GiB.

A buffer is registered in one of three ways:

.. list-table::
   :header-rows: 1
   :widths: 22 30 18 30

   * - Registered with
     - How
     - Address in tokens
     - Extra work per transfer
   * - ``hipObjBufRegister()``, directly
     - dma-buf MR, or a plain MR if that succeeds
     - The caller's address
     - ``hipDeviceSynchronize()`` after the RDMA
   * - ``hipObjBufRegister()``, staged
     - Plain MR over a pinned host buffer of the same size
     - The staging buffer's
     - A copy to the staging buffer before a PUT, or from it after a
       GET
   * - ``hipObjBufRegisterHost()``
     - Plain MR over the caller's memory
     - The caller's address
     - None

For memory that HIP reports as device memory, ``hipObjBufRegister()``
first checks that the range lies within one HIP allocation.
``registerDeviceMemory()`` then tries, in order:

#. If dma-buf registration is enabled (``HIPOBJ_DMABUF``) and
   libibverbs supports it: export the range as a dma-buf through HSA,
   and register it with ``ibv_reg_dmabuf_mr()``, using the caller's
   address as the I/O virtual address. The dma-buf fd is closed as
   soon as the call returns, since the MR keeps its own reference.
   Whether this works is found out by trying, not predicted from
   kernel configuration.
#. A plain ``ibv_reg_mr()``, which works for host memory and
   generally fails for device memory.
#. If both fail for device memory: allocate a pinned host staging
   buffer with ``hipHostMalloc()`` and register that, after a warning
   on stderr that the buffer is no longer GPU-direct.
   ``HIPOBJ_REQUIRE_GPU_DIRECT`` makes this a failure instead, so
   that CI can't pass on the fallback without noticing.

Every MR gets local write, remote read, and remote write access,
whatever the direction of the transfers that will use it.

Registration runs without the map's lock: the map is checked first
under a shared lock, so a duplicate or a full map doesn't register
for nothing, and checked again when the entry is inserted under the
exclusive lock. If two threads register the same address, the
loser's registration is released after the lock.

A ``BufEntry`` declares the staging buffer before the MR, so the MR
is deregistered before the memory it covers is freed.

``deregisterBuffer()`` refuses, with ``hipObjBusy``, to remove an
entry that anything else holds a copy of: a call that found it with
``find()``, or an abandoned copy. Since copies are only made under
the map's lock, the check can't miss one. The MR and staging buffer
are released after the lock.

A Transfer
----------

``hipObjGet()`` and ``hipObjPut()`` (``src/hipobj.cpp``) share
``runRdmaTransfer()``. With the instance found and the arguments
checked:

#. **Look up the buffer** with ``buffers.find()``, check that
   ``[offset, offset + size)`` lies inside it without computing the
   sum, and refuse a staged buffer with ``hipObjBusy`` while an
   abandoned copy of it is still running (see `Staging Copies`_).
#. **PUT, staged:** copy the range from the device to the staging
   buffer.
#. **Lease a connection** from the pool.
#. **Mint the token:** the lease's QP number, the device's GID, the
   MR's rkey, the buffer's token address plus the offset, the size,
   RC, the port number, and a LID of 0. It's encoded as 88 lowercase
   hex digits (44 little-endian bytes, laid out in :doc:`interop`)
   into a NUL-terminated array on the stack.
#. **sendRequest(ctx, token, 88):** the application embeds the token
   in its S3 request.
#. **recvReply(ctx, buffer, &length):** the application returns the
   ``x-amz-rdma-reply`` value, into a 512-byte buffer on the stack.
   Anything but a success reply (``ok``, 200, 204, or 206), including
   501, which means the server declined RDMA, fails the call with
   ``hipObjS3Error``. Falling back to plain HTTP is the
   application's job; it has the reply.
#. **Connect**, if the reply has the form ``200:<88 hex digits>``, to
   the peer QP that token names. ``connectRcPeer()`` uses only the
   token's transport (which must be RC), QP number, GID, and LID; the
   other fields are reserved in a reply (see :doc:`interop`).
#. **Wait for a completion** on the lease's CQ, for up to 5 seconds.
#. **Synchronize the device**, for a directly registered GPU buffer.
#. **End the lease**, which resets the QP if it was connected and
   returns it to the pool.
#. **GET, staged:** copy the range from the staging buffer to the
   device.

The ``hipObjHandle_t`` argument is unused, and neither the token nor
the transfer records whether it's a GET or a PUT: the server knows
that from the S3 request.

No lock is held during the callbacks, so they can call any public
function. They run on the calling thread.

.. code-block:: text

   caller          hipObject                 app's S3 client       S3 server
     |                 |                            |                   |
     |-- hipObjGet --->| find buffer, lease conn,   |                   |
     |                 | mint token                 |                   |
     |                 |-- sendRequest(token) ----->|-- GET + token --->|
     |                 |                            |   RDMA WRITE into |
     |                 |                            |   the buffer      |
     |                 |<-- recvReply(reply) -------|<-- reply ---------|
     |                 | parse reply, connect QP,   |                   |
     |                 | poll CQ, sync device,      |                   |
     |                 | return lease               |                   |
     |                 | (staged: staging -> GPU)   |                   |
     |<-- result ------|                            |                   |

The diagram shows the server doing its RDMA before it replies, which
is what the in-repo test server (``hipobj-rdma-test-server``) does;
see `Open Questions`_ about that ordering.

Transfers the Application Drives
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The MinIO bridge, and any application that doesn't fit the callback
model, uses the lower-level calls instead:

- ``hipObjGetRdmaToken()`` mints a token for the start of a
  registered buffer, on the token connection (see
  `The Token Connection`_), and returns a ``malloc()``\ ed copy that
  ``hipObjPutRdmaToken()`` frees.
- ``hipObjBufSync()`` performs the staging copy that ``hipObjPut()``
  and ``hipObjGet()`` would (``HIPOBJ_SYNC_TO_HOST`` before a PUT,
  ``HIPOBJ_SYNC_TO_DEVICE`` after a GET), and does nothing for a
  buffer that isn't staged.
- ``hipObjParseRdmaReply()`` turns an ``x-amz-rdma-reply`` value into
  its HTTP status.
- ``hipObjTokenClientNic()`` extracts the IPv4 address from a token's
  GID, when the GID is IPv4-mapped.

These calls don't connect a QP or wait for a completion. The last
two, like ``hipObjGetErrorString()``, ``hipObjGetVersionString()``,
and ``hipObjGetVersion()``, don't need an initialized library.

Tokens and Replies
^^^^^^^^^^^^^^^^^^

``src/rdma/token.cpp`` serializes a token field by field, with
explicit little-endian byte order, never by copying a struct, and a
``static_assert`` fails the build if a field's size changes the wire
format. Decoding accepts exactly 88 hex digits in either case and a
transport byte of DC or RC, and nothing else. A reply is ``ok``,
``err``, a three-digit status from 100 to 599, or ``200:`` followed
by a token; trailing NUL, CR, and LF characters are ignored, and
anything else is rejected rather than guessed at.

Staging Copies
--------------

A staging copy (``stageCopyWithDeadline()`` in ``src/hipobj.cpp``)
is a ``hipMemcpyAsync()`` on the calling thread's per-thread default
stream (so copies from different threads don't serialize on the null
stream), with an event recorded after it. The event is polled every
millisecond until the copy finishes or ``HIPOBJ_STAGE_TIMEOUT_MS``
(default 30 s) passes.

The timeout exists because ROCm waits on a copy by spinning in user
space: a DMA that never completes would otherwise hang the caller
forever. But a copy that timed out can't be stopped, and can still
read or write the staging buffer whenever the GPU gets to it. So the
copy's event and the buffer's ``BufEntry`` are handed to
``AbandonedCopies`` (``src/abandoned-copies.h``):

- Holding the entry keeps the staging buffer allocated, and makes
  deregistration report ``hipObjBusy``.
- Transfers, syncs, and token requests on that buffer return
  ``hipObjBusy`` while the copy is running, since they'd mix their
  data with its.
- Copies that have finished are released when they're next checked:
  by a call on the same buffer, or by any ``hipObjBufDeregister()``.
- If the instance is destroyed while a copy is still running, its
  event and staging buffer are leaked rather than freed under it.

Handing a copy over must not fail, or a running copy would be left
untracked. So each copy reserves room in the list (``reserve()``,
which returns a ``Slot``) before it starts, and ``add()`` uses that
room without allocating.

Concurrency
-----------

The caller-facing contract is in the header's ``threads`` group (see
:doc:`api`), and the rules for keeping the code in line with it are
in ``AGENTS.md``. In short:

.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * - Lock
     - Guards
     - Held
   * - ``Current::initMutex``
     - Init and shutdown, one at a time
     - Throughout ``initWith()`` and ``shutdownInstance()``
   * - ``Current::mutex`` (shared)
     - The current instance pointer
     - Shared to copy it, exclusive to change it
   * - ``BufferMap::mutex_`` (shared)
     - The map of entries
     - Shared for lookups, exclusive to insert or remove
   * - ``ConnectionPool::mutex_``
     - The free list, live count, and closed flag
     - To lease or return a connection. A thread waiting for one
       releases it while it waits, and takes it back to check the
       pool again.
   * - ``AbandonedCopies::mutex_``
     - The list of copies and reservations
     - To reserve room or add a copy, and while querying the copies'
       events, which doesn't wait

Locks guard lookups, not work: a lookup copies a ``std::shared_ptr``
(or takes a ``Lease``) and releases the lock, and no lock is held
across a callback, a verbs call, or a HIP call, apart from the
exceptions in the table. They're the annotated wrappers in
``src/common/thread-safety.h``, so Clang's ``-Wthread-safety`` checks
what each one guards and the order ``Current``'s two are taken in.

The one wait in a transfer is for a connection, and the thread-local
lease count turns the wait that could deadlock, a callback waiting
for its own connection, into ``hipObjBusy``.

Errors
------

Every public function but ``hipObjGetErrorString()`` and
``hipObjGetVersionString()``, which return strings, returns a
``hipObjError_t``, built with ``makeError()``
(``src/hipobj-private.h``): an operation error, and the HIP error
behind it where there is one. Each public function body is a function
``try`` block, so no exception crosses the C API. The functions that
return a ``hipObjError_t`` turn any exception, whatever its type,
into ``hipObjInternalError``; the two that return strings return a
fallback string instead (``"Unknown error"`` and ``"0.0.0"``).

Internal functions report failure with ``bool``, ``std::optional``,
or a result ``enum class``, and ``hipobj.cpp`` maps those to error
codes. Details that an error code can't carry, such as a failed work
completion's status or a fallback to a staging buffer, go to stderr;
the hot path prints nothing on success.

Configuration
-------------

``parseOptions()`` reads these environment variables once, in
``hipObjInit()``. An unset or invalid value leaves the default.

.. list-table::
   :header-rows: 1
   :widths: 32 12 56

   * - Variable
     - Default
     - Effect
   * - ``HIPOBJ_MAX_TRANSFERS``
     - 16
     - Connections in the pool (1 to 1024), so how many transfers
       run at once
   * - ``HIPOBJ_STAGE_TIMEOUT_MS``
     - 30000
     - How long a staging copy can take (up to 24 hours)
   * - ``HIPOBJ_DMABUF``
     - on
     - Values starting ``0``, ``n``, or ``N`` turn off dma-buf
       registration
   * - ``HIPOBJ_REQUIRE_GPU_DIRECT``
     - off
     - Any value but one starting ``0``, ``n``, or ``N`` makes the
       staging fallback a failure
   * - ``HIPOBJ_RDMA_DEBUG``
     - off
     - Any value not starting ``0`` reports failed dma-buf exports

The completion timeout, 5 seconds, isn't configurable.

Open Questions
--------------

The code doesn't show how these are meant to work:

When the client's QP is connected
   A transfer connects its QP to the peer only after ``recvReply()``
   returns, that is, after the S3 response has arrived, and the token
   connection is never connected at all. An RC QP in INIT doesn't
   respond to incoming RDMA, yet the test server connects its own QP
   and does its RDMA before it sends the reply. Issue #22 reported
   this ordering as a defect; it was closed by the "v2" two-phase
   protocol, which was later removed (#203). The emulated-hardware CI
   lanes assert that RDMA transfers succeed, including through the
   MinIO bridge, which uses the never-connected token connection, so
   it isn't clear whether real NICs work with this ordering, or how
   they're expected to.

Which completion a transfer waits for
   The client posts no work requests, and on an RC QP, one-sided RDMA
   reads and writes by the peer don't generate completions on the
   responder's CQ. ``pollCompletion()`` waits for any completion and
   treats a timeout as failure. The comment in
   ``finishTransferAfterReply()`` says the check reports "no evidence
   of failure" rather than a verified transfer, but it isn't clear
   which completion is expected to arrive in a successful transfer.
