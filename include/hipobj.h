/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>

#if defined(__GNUC__)
#define HIPOBJ_API __attribute__((visibility("default")))
#else
#define HIPOBJ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 *
 * @mainpage hipObject API Reference
 *
 * @section contents Contents
 * - @ref core
 * - @ref error
 * - @ref buffer
 * - @ref io
 */

/*!
 * @defgroup core Core Functionality
 * @defgroup error Errors and Error Handling
 * @defgroup buffer Buffer Registration
 * @defgroup io Data Transfer (GET / PUT)
 */

/* -------------------------------------------------------
 *  LIBRARY VERSION NUMBERS
 * ------------------------------------------------------- */

/*! @brief hipObject major version number @ingroup core */
#define HIPOBJ_VERSION_MAJOR 0
/*! @brief hipObject minor version number @ingroup core */
#define HIPOBJ_VERSION_MINOR 1
/*! @brief hipObject patch version number @ingroup core */
#define HIPOBJ_VERSION_PATCH 0

/* -------------------------------------------------------
 *  ERROR HANDLING
 * ------------------------------------------------------- */

/*! @brief Base value for hipObject error codes @ingroup error */
#define HIPOBJ_BASE_ERR 6000

/*!
 * @brief Operation-level error codes
 * @ingroup error
 */
typedef enum {
  hipObjSuccess = 0,
  hipObjInvalidValue,
  hipObjNotInitialized,
  hipObjAlreadyInitialized,
  hipObjRdmaError,
  hipObjS3Error,
  hipObjBufNotRegistered,
  hipObjBufAlreadyRegistered,
  hipObjNicNotFound,
  hipObjDmabufNotSupported,
  hipObjSizeTooLarge,
  hipObjInternalError,
  hipObjNotSupported, /*!< Server explicitly does not support hipobj-rc-v2 */
  hipObjBusy,         /*!< Server backpressure (503) and retries exhausted */
} hipObjOpError_t;

/*!
 * @brief Compound error type carrying both an
 *        operation error and an optional HIP error
 * @ingroup error
 */
typedef struct {
  hipObjOpError_t opError;
  int hipError;
} hipObjError_t;

#define HIPOBJ_SUCCESS ((hipObjError_t){hipObjSuccess, 0})

/*! @brief Diagnostic marker in hipObjError_t::hipError when a
 * transfer failed hipObjBusy because its deadline expired.
 * Distinguishes expiry from server-side backpressure.
 * @ingroup error */
#define HIPOBJ_DIAG_DEADLINE_EXPIRED 0x54494D45 /* "TIME" */

/*!
 * @brief Return a human-readable string for an
 *        operation error code
 * @ingroup error
 */
HIPOBJ_API const char* hipObjGetErrorString(hipObjOpError_t err);

/* -------------------------------------------------------
 *  TYPES
 * ------------------------------------------------------- */

/*!
 * @brief Opaque handle to an S3 object for RDMA I/O
 * @ingroup core
 */
typedef void* hipObjHandle_t;

/*!
 * @brief Callback struct for S3 SDK integration.
 *
 * The application provides these callbacks so that
 * hipObject can inject RDMA tokens into S3 requests
 * and receive RDMA reply tags from the server.  This
 * mirrors cuObject's CUObjOps_t pattern.
 *
 * @ingroup core
 */
typedef struct {
  /*!
   * Called by hipObject to send the RDMA token.
   * The application should embed the token in the
   * x-amz-rdma-token S3 request header.
   *
   * @param ctx       User-supplied context pointer
   * @param token     Hex-encoded RDMA token
   * @param tokenLen  Length of token in bytes
   * @return 0 on success, negative on failure
   */
  int (*sendRequest)(void* ctx, const char* token, size_t tokenLen);

  /*!
   * Called by hipObject to receive the RDMA reply.
   * The application should extract x-amz-rdma-reply
   * from the S3 response headers.
   *
   * @param ctx       User-supplied context pointer
   * @param reply     Buffer to receive the reply tag
   * @param replyLen  In: buffer size; out: actual length
   * @return 0 on success, negative on failure
   */
  int (*recvReply)(void* ctx, char* reply, size_t* replyLen);
} hipObjOps_t;

/*!
 * @brief Configuration for hipObject initialization
 * @ingroup core
 */
typedef struct {
  const char* endpoint;  /*!< S3 endpoint URL           */
  const char* region;    /*!< AWS region                */
  const char* accessKey; /*!< S3 access key (optional)  */
  const char* secretKey; /*!< S3 secret key (optional)  */
  uint32_t flags;        /*!< Reserved, set to 0        */
  int gpuDevice;         /*!< HIP device index, or -1   */
  const char* nicHint;   /*!< NIC name hint, or NULL    */
} hipObjConfig_t;

/*! @brief RDMA token operation: server RDMA READ (client PUT) @ingroup io */
#define HIPOBJ_RDMA_OP_PUT 0
/*! @brief RDMA token operation: server RDMA WRITE (client GET) @ingroup io */
#define HIPOBJ_RDMA_OP_GET 1

/*! @brief x-amz-rdma-reply: server declined RDMA (HTTP fallback) @ingroup io */
#define HIPOBJ_RDMA_REPLY_NOT_IMPLEMENTED 501

/* -------------------------------------------------------
 *  CORE LIFECYCLE
 * ------------------------------------------------------- */

/*!
 * @brief Initialize the hipObject library
 *
 * Opens the RDMA device, selects the closest NIC to
 * the GPU, and prepares internal state.  Must be
 * called before any other hipObj* function.
 *
 * @param config  Pointer to configuration struct
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjInit(hipObjConfig_t* config);

/*!
 * @brief Shut down the hipObject library
 *
 * Releases all RDMA resources.  All registered
 * buffers are implicitly deregistered.
 *
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjShutdown(void);

/* -------------------------------------------------------
 *  BUFFER REGISTRATION
 * ------------------------------------------------------- */

/*!
 * @brief Register a GPU buffer for RDMA transfers
 *
 * Exports the GPU buffer via dmabuf and registers it
 * with the RDMA NIC.  Maximum 4 GiB per registration.
 *
 * @param devPtr  Pointer returned by hipMalloc
 * @param size    Size of the buffer in bytes
 * @return hipObjError_t
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufRegister(void* devPtr, size_t size);

/*!
 * @brief Deregister a previously registered GPU buffer
 *
 * @param devPtr  Pointer previously passed to
 *                hipObjBufRegister
 * @return hipObjError_t
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufDeregister(void* devPtr);

/* -------------------------------------------------------
 *  DATA TRANSFER
 * ------------------------------------------------------- */

/*!
 * @brief GET: fetch an S3 object into GPU memory via
 *        RDMA
 *
 * The server performs an RDMA WRITE to push data into
 * the registered GPU buffer.
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the S3 object
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGet(hipObjHandle_t handle, void* devPtr,
                                   size_t size, off_t offset, hipObjOps_t* ops,
                                   void* ctx);

/*!
 * @brief PUT: store GPU memory to an S3 object via
 *        RDMA
 *
 * The server performs an RDMA READ to pull data from
 * the registered GPU buffer.
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the S3 object
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPut(hipObjHandle_t handle, const void* devPtr,
                                   size_t size, off_t offset, hipObjOps_t* ops,
                                   void* ctx);

/* -------------------------------------------------------
 *  hipobj-rc-v2 (TWO-ROUND-TRIP CONTROL PROTOCOL)
 * ------------------------------------------------------- */

/*!
 * @brief V2 control endpoint settings
 * @ingroup core
 *
 * The v2 protocol runs its prepare/ready/cancel exchange on a dedicated
 * control endpoint; there is no default port, the caller must supply one.
 */
typedef struct {
  const char* controlEndpoint; /*!< Required "http(s)://host:port" URI;
                                    the library copies the string */
} hipObjControlEndpointV2_t;

/*!
 * @brief V2 initialization configuration
 * @ingroup core
 *
 * Zero-initialize the struct (or value-initialize with {}) and set
 * only the fields you need; every added field has a well-defined
 * default of 0 meaning "library default". There is no struct
 * versioning: consumers must rebuild against the header they run
 * with.
 */
typedef struct {
  hipObjConfig_t v1;                 /*!< All v1 fields */
  hipObjControlEndpointV2_t control; /*!< v2 control endpoint (required) */
  uint32_t connectDeadlineMs;        /*!< 0 selects 10 s. Bounds local RTR/RTS
                                          transitions and is capped by the
                                          transfer deadline. */
  uint32_t transferDeadlineMs;       /*!< 0 = default (60 s). Whole-transfer
                                          budget from entry to hipObjGetV2/
                                          PutV2, including lock/admission
                                          wait, all control exchanges, and
                                          the data phase. */
  uint32_t cancelCleanupBudgetMs;    /*!< 0 = default (1 s). Budget for the
                                       single post-expiry CANCEL attempt
                                       and local teardown; never extends
                                       the transfer result past TIMEOUT. */
} hipObjConfigV2_t;

/* Forward declaration of the phase-aware callback set (see below). */
typedef struct hipObjOpsV2 hipObjOpsV2_t;

/*!
 * @brief Per-transfer request description for the v2 phases
 * @ingroup io
 *
 * Borrow contract: string fields are owned by the library and remain
 * valid for the duration of a single callback invocation. Callbacks are
 * synchronous (they return before the library continues) and must copy
 * anything they need to keep.
 */
typedef struct {
  const char* method;   /*!< "GET" or "PUT" */
  const char* bucket;   /*!< Object bucket */
  const char* key;      /*!< Object key */
  const char* query;    /*!< Canonical query string or NULL */
  const char* token;    /*!< 88-hex token[:addr:size] */
  const char* session;  /*!< READY/cancel: session id (library sets) */
  const char* target;   /*!< Canonical rdma-target value (library sets) */
  uint64_t size;        /*!< Transfer size in bytes */
  uint64_t offset;      /*!< Byte offset into the object */
  uint32_t cookie;      /*!< Client cookie (library generates) */
  uint32_t clientPsn;   /*!< Client PSN, 1..0xffffff (library generates) */
  uint32_t deadlineMs;  /*!< Whole-transfer budget remaining, set by the
                            library before every callback. Zero means
                            the budget is exhausted; callbacks should
                            fail fast. */
  uint32_t remainingMs; /*!< Budget relevant to the current callback:
                            the transfer's remaining budget for in-
                            flight phases, or the fresh cleanup budget
                            for the post-expiry CANCEL. */
  const hipObjControlEndpointV2_t* endpoint; /*!< Control endpoint (library sets
                                                from init) */
  const char* nic;       /*!< RDMA device the data plane selected (library
                              sets from init; NULL before init) */
  int nicPort;           /*!< Selected port number, 0 when unknown */
  int nicGidIndex;       /*!< Selected GID index, -1 when unknown */
  uint32_t clientQpn;    /*!< READY: this transfer's QP number (hex). The
                             server pairs its QP from it; zero on phases
                             that run before the endpoint exists. */
  uint64_t clientMrAddr; /*!< READY: registered buffer address (hex).
                             For GET the server WRITEs the object into
                             it; for PUT it is bookkeeping only. Zero
                             before READY. */
  uint32_t clientMrRkey; /*!< READY: registered buffer rkey (hex) */
} hipObjTransferReqV2_t;

/*! @brief Response to PREPARE @ingroup io */
typedef struct {
  int httpStatus;        /*!< Status code (200/501/403/413/503/500) */
  int protocolEcho;      /*!< 1 when X-Amz-Rdma-Protocol: hipobj-rc-v2 seen */
  int unsupportedMarker; /*!< 1 when the explicit unsupported marker seen */
  char serverToken[97];  /*!< 88-hex peer token + NUL */
  char session[65];      /*!< 32-hex session id + NUL */
  uint32_t serverPsn;    /*!< Server PSN, 1..0xffffff (0 = invalid) */
  uint64_t stagingAddr;  /*!< Server staging MR address (PUT), 0 when
                             absent; required for PUT data delivery */
  uint32_t stagingRkey;  /*!< Server staging MR rkey (PUT), 0 when
                             absent */
  int stagingPresent;    /*!< 1 when both staging fields were present
                             and valid; a PUT without a valid staging
                             advertisement fails the transfer */
} hipObjPrepareReplyV2_t;

/*! @brief FINAL response (the reply to READY) @ingroup io */
typedef struct {
  int httpStatus;   /*!< 200 (GET; PUT accepts 200 or 204) / 5xx / 409 / 408 */
  int protocolEcho; /*!< 1 when the protocol echo header was present */
  uint64_t bytes;   /*!< Bytes transferred per the server */
  uint32_t cookieEcho;  /*!< Must match the request cookie */
  char etag[128];       /*!< S3 ETag when present, else empty */
  char versionId[128];  /*!< S3 version id when present, else empty */
  char checksumB64[13]; /*!< 12-char canonical CRC64NVME base64 or empty */
  int cookiePresent;    /*!< 1 when the cookie echo header was present */
} hipObjFinalReplyV2_t;

/*!
 * @brief Phase-aware callbacks for the v2 control protocol
 * @ingroup io
 *
 * sendPrepare and sendCancel each perform one complete HTTP round
 * trip on the control endpoint. The READY exchange is split:
 * sendReadyRequest writes the request bytes and returns, the data
 * phase runs while the exchange is pending, then finishReady reads
 * the response. Callbacks must not re-enter the library (the
 * library-wide lock is not recursive). All callbacks are required
 * for v2 transfers. The v1 member is unused by the v2 entry points
 * and is kept for structural forward compatibility.
 *
 * Time budget: the transfer deadline covers everything a callback
 * does, including credential acquisition and DNS resolution. The
 * reference bridge caches signed exchanges and bounds its resolver
 * by the deadline; integrations must supply a credential provider
 * that is already cached or otherwise bounded, because the library
 * cannot interrupt a synchronous provider fetch from inside the
 * callback.
 */
typedef struct hipObjOpsV2 {
  hipObjOps_t v1;

  /*! Issue PREPARE; out is filled from the response headers. */
  int (*sendPrepare)(void* ctx, const hipObjTransferReqV2_t* req,
                     hipObjPrepareReplyV2_t* out);

  /*! Issue the READY request: write the request bytes to the
   * transport and return once they are flushed. Does NOT read the
   * response. Success means the complete request was handed to the
   * transport; a partial write or I/O error must be reported as a
   * failure (the exchange is then aborted and finishReady is never
   * called for it). */
  int (*sendReadyRequest)(void* ctx, const hipObjTransferReqV2_t* req);

  /*! Read the READY response (FINAL) and fill out. Called at most
   * once, and exactly once when sendReadyRequest succeeded and the
   * exchange reaches a terminal disposition. On a data-phase failure
   * or budget expiry the consumer should abort (close) the
   * connection instead; an aborted connection must never be reused
   * or returned to a pool. */
  int (*finishReady)(void* ctx, const hipObjTransferReqV2_t* req,
                     hipObjFinalReplyV2_t* out);

  /*! Issue CANCEL (idempotent). Only the HTTP status matters. */
  int (*sendCancel)(void* ctx, const hipObjTransferReqV2_t* req);
} hipObjOpsV2_t;

/*!
 * @brief Initialize the library for hipobj-rc-v2 transfers
 * @ingroup core
 *
 * Mutually exclusive with hipObjInit: whichever is called first wins and
 * the other returns hipObjAlreadyInitialized until hipObjShutdown.
 */
HIPOBJ_API hipObjError_t hipObjInitV2(hipObjConfigV2_t* config);

/*!
 * @brief Query the RDMA NIC name selected by hipObjInitV2
 * @ingroup init
 *
 * Returns a freshly allocated copy of the device name chosen at init
 * (the config hint when one was given), or NULL before hipObjInitV2
 * has run or on allocation failure. The snapshot is taken under the
 * library lock, so concurrent init/shutdown cannot mutate the storage
 * while it is read. Release with hipObjFreeNicV2. This is the
 * supported way for v2 consumers to learn the NIC; no v1 RDMA token
 * is needed.
 */
HIPOBJ_API char* hipObjNicV2(void);

/*! @brief Release a NIC name snapshot from hipObjNicV2 @ingroup init */
HIPOBJ_API void hipObjFreeNicV2(char* nic);

/*! @brief Port the v2 data plane selected at init @ingroup init
 *
 * 1-based RDMA port number; 0 before init or when unavailable. With
 * hipObjSelectedGidIndexV2 this identifies the exact sysfs ndevs
 * entry backing the data-plane address handle, so the control plane
 * can bind the same interface (multi-port or VLAN-GID devices never
 * silently bind an arbitrary sibling).
 */
HIPOBJ_API int hipObjSelectedPortV2(void);

/*! @brief GID index the v2 data plane selected at init @ingroup init
 *
 * -1 before init or when unavailable. Pair with
 * hipObjSelectedPortV2 and the NIC from hipObjNicV2 to read
 * @c /sys/class/infiniband/<dev>/ports/<port>/gid_attrs/ndevs/<gid>.
 */
HIPOBJ_API int hipObjSelectedGidIndexV2(void);

/*!
 * @brief Coherent snapshot of the data plane's interface selection
 * @ingroup init
 *
 * Copies the NIC name, port number, and GID index selected at init
 * into the caller-provided storage in one library-internal step, so
 * the triple cannot mix values from different initializations when
 * read while a shutdown/reinit is in flight. The name is truncated
 * to fit. Returns 1 when an initialized selection was copied, 0
 * when the library is not initialized.
 */
HIPOBJ_API int hipObjInterfaceSnapshotV2(char* nicOut, size_t nicLen,
                                         int* portOut, int* gidOut);

/*!
 * @brief V2 GET: download an object into a registered buffer
 * @ingroup io
 *
 * Runs the two-round-trip protocol (PREPARE, then READY whose response
 * is FINAL) against the configured control endpoint. The transfer size
 * is capped at 2^31-1 bytes; larger requests fail with
 * hipObjSizeTooLarge. When the server answers PREPARE with 501 plus the
 * protocol-unsupported marker the function returns hipObjNotSupported
 * and the caller may fall back to plain HTTP; failures after READY are
 * never retried or fallen back. The session lifetime is the function
 * scope: on return the session is terminated and the connection
 * quiesced.
 *
 * Buffer lifetime on error: when a transfer fails to quiesce (the
 * return carries hipObjInternalError and the message mentions the
 * release failure), the connection that accessed the buffer remains
 * owned by the library and pins the buffer's memory region. The
 * buffer must not be reused, freed, or re-registered until a
 * successful hipObjBufDeregister or hipObjShutdown completes;
 * hipObjBufDeregister refuses while such a pin exists.
 */
HIPOBJ_API hipObjError_t hipObjGetV2(const char* bucket, const char* key,
                                     void* devPtr, uint64_t size,
                                     uint64_t offset, const char* query,
                                     hipObjOpsV2_t* ops, void* ctx);

/*!
 * @brief V2 PUT: upload GPU memory to an S3 object via RDMA
 * @ingroup io
 *
 * Same contract as hipObjGetV2. The server's PREPARE reply must
 * advertise a valid staging memory region; the client pushes the
 * data with RDMA WRITE while the READY exchange is pending.
 *
 * Device-only registration policy: the v2 entry points require the
 * buffer to be registered with a device memory region. Host-MR
 * substitution is rejected for v2 transfers.
 */
HIPOBJ_API hipObjError_t hipObjPutV2(const char* bucket, const char* key,
                                     const void* devPtr, uint64_t size,
                                     uint64_t offset, const char* query,
                                     hipObjOpsV2_t* ops, void* ctx);

/*!
 * @brief Mint a hex-encoded RC RDMA token for a registered buffer
 *
 * The caller must release @p *outToken with hipObjPutRdmaToken().
 * @p op is HIPOBJ_RDMA_OP_PUT or HIPOBJ_RDMA_OP_GET (reserved).
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGetRdmaToken(const void* devPtr, size_t size,
                                            int op, char** outToken);

/*!
 * @brief Release a token allocated by hipObjGetRdmaToken()
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPutRdmaToken(char* token);

/*!
 * @brief Parse an x-amz-rdma-reply header value
 *
 * On success writes the HTTP-style reply code (200, 204, 206, 501)
 * to @p httpCode.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjParseRdmaReply(const char* reply,
                                              size_t replyLen, int* httpCode);

/*!
 * @brief Extract client NIC IPv4 from a minted RDMA token
 *
 * Writes a dotted-quad address into @p nicIp when the token GID
 * carries an IPv4-mapped RoCEv2 suffix.  Returns hipObjSuccess with
 * an empty string when no address is encoded.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjTokenClientNic(const char* token, char* nicIp,
                                              size_t nicIpLen);

/*!
 * @brief Return the library version as a string
 * @ingroup core
 */
HIPOBJ_API const char* hipObjGetVersionString(void);

#ifdef __cplusplus
}
#endif
