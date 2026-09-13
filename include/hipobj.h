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
 * - @ref threads
 * - @ref error
 * - @ref buffer
 * - @ref io
 * - @ref v2
 */

/*!
 * @defgroup core Core Functionality
 * @defgroup error Errors and Error Handling
 * @defgroup buffer Buffer Registration
 * @defgroup io Data Transfer (GET / PUT)
 */

/*!
 * @defgroup threads Thread Safety
 *
 * hipObject can be called from any number of threads. Each function's
 * documentation says which of these it is:
 *
 * - **Serialized** functions use the library's state: whether it's
 *   initialized, the registered buffers, and the RDMA connection. They
 *   run one at a time across the whole process. Each one takes a
 *   library-wide lock and holds it until it returns, so a serialized
 *   call waits for the one in progress, on any thread, to finish.
 * - **Concurrent** functions don't use the library's state. They can be
 *   called from any thread at any time, including while a serialized
 *   function runs and from a callback. As with any C function, calls that
 *   run at the same time mustn't share a buffer that one of them writes
 *   to, or a token that one of them releases.
 *
 * Since serialized calls run one at a time, only one transfer is in
 * progress at a time, and calling hipObject from more threads doesn't
 * move data any faster. A call can also wait a long time for another
 * thread's transfer, which holds the lock for as long as the transfer
 * takes: while the application's callbacks send the S3 request and wait
 * for the response, while hipObject copies the buffer through host
 * memory (for a buffer that the NIC can't reach directly), and while it
 * waits for the RDMA transfer to complete.
 *
 * hipObjShutdown() waits for the serialized call in progress to finish.
 * Calls made after it, on any thread, that need an initialized library
 * fail with hipObjNotInitialized until hipObjInit() is called again.
 *
 * **Callbacks.** hipObjGet() and hipObjPut() run the hipObjOps_t
 * callbacks with the lock held. A callback can call concurrent
 * functions. If it calls a serialized function, that function returns
 * hipObjInvalidValue instead of waiting for a lock its own thread holds.
 * A callback also mustn't wait for another thread that's calling a
 * serialized function, since that thread is waiting for the lock:
 * neither can continue, and hipObject can't detect it.
 *
 * **Transfers made of several calls.** The lock covers one call, not a
 * sequence of calls. A transfer that the application drives itself,
 * with hipObjGetRdmaToken(), its own S3 request, and hipObjBufSync(), is
 * several calls, and other threads' calls can run between them.
 * hipObject uses one RDMA connection for every transfer, so the
 * application must make sure that such a transfer doesn't overlap any
 * other transfer, on any thread.
 *
 * Serializing the library is how hipObject works now, not a promise: a
 * later release may run transfers concurrently.
 */

/* The generated documentation leaves out the warning below, through the
 * undefined HIPOBJ_HEADER_ONLY label, and shows the copy in docs/api.rst
 * at the top of the section instead. Breathe would move this one to the
 * end of the description. */
/*!
 * @defgroup v2 Experimental V2 API (hipobj-rc-v2)
 *
 * @if HIPOBJ_HEADER_ONLY
 * @warning The V2 API is experimental. It may change incompatibly or be
 * removed in any release. hipObjGetV2() and hipObjPutV2() are not
 * implemented yet and return hipObjNotSupported.
 * @endif
 *
 * The V2 API runs each transfer over the hipobj-rc-v2 control protocol:
 * two round trips on a dedicated control endpoint, PREPARE and then READY,
 * whose response is FINAL. CANCEL abandons a prepared transfer.
 *
 * These declarations, and the hipObjNotSupported and hipObjBusy error
 * codes, only exist when HIPOBJECT_V2_API is defined. The
 * HIPOBJECT_V2_API CMake option (ON by default) builds the V2 API and
 * defines the macro for everything that links hipobj::hipobj.
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
#ifdef HIPOBJECT_V2_API
    hipObjNotSupported, /*!< V2 API: server explicitly does not support hipobj-rc-v2 */
    hipObjBusy,         /*!< V2 API: server backpressure (503) and retries exhausted */
#endif
} hipObjOpError_t;

/*!
 * @brief Compound error type carrying both an
 *        operation error and an optional HIP error
 * @ingroup error
 */
typedef struct {
    hipObjOpError_t opError;
    int             hipError;
} hipObjError_t;

/*! @brief Diagnostic marker in hipObjError_t::hipError when a
 * transfer failed hipObjBusy because its deadline expired.
 * Distinguishes expiry from server-side backpressure.
 * @ingroup error */
#define HIPOBJ_DIAG_DEADLINE_EXPIRED 0x54494D45 /* "TIME" */

/*!
 * @brief Return a human-readable string for an
 *        operation error code
 * @concurrent
 * @ingroup error
 */
HIPOBJ_API const char *hipObjGetErrorString(hipObjOpError_t err);

/* -------------------------------------------------------
 *  TYPES
 * ------------------------------------------------------- */

/*!
 * @brief Opaque handle to an S3 object for RDMA I/O
 * @ingroup core
 */
typedef void *hipObjHandle_t;

/*!
 * @brief Callback struct for S3 SDK integration.
 *
 * The application provides these callbacks so that
 * hipObject can inject RDMA tokens into S3 requests
 * and receive RDMA reply tags from the server.  This
 * mirrors cuObject's CUObjOps_t pattern.
 *
 * The callbacks run while hipObject holds its API lock,
 * so they can't call serialized functions; see
 * @ref threads.
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
    int (*sendRequest)(void *ctx, const char *token, size_t tokenLen);

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
    int (*recvReply)(void *ctx, char *reply, size_t *replyLen);
} hipObjOps_t;

/*!
 * @brief Configuration for hipObject initialization
 * @ingroup core
 */
typedef struct {
    const char *endpoint;  /*!< S3 endpoint URL           */
    const char *region;    /*!< AWS region                */
    const char *accessKey; /*!< S3 access key (optional)  */
    const char *secretKey; /*!< S3 secret key (optional)  */
    uint32_t    flags;     /*!< Reserved, set to 0        */
    int         gpuDevice; /*!< HIP device index, or -1   */
    const char *nicHint;   /*!< NIC name hint, or NULL    */
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
 * @serialized
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjInit(hipObjConfig_t *config);

/*!
 * @brief Shut down the hipObject library
 *
 * Releases all RDMA resources.  All registered
 * buffers are implicitly deregistered.
 *
 * @return hipObjError_t
 * @serialized
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
 * The buffer must lie within a single HIP allocation:
 * returns hipObjInvalidValue if it runs past the end
 * of the allocation that holds devPtr.
 *
 * @param devPtr  Pointer returned by hipMalloc
 * @param size    Size of the buffer in bytes
 * @return hipObjError_t
 * @serialized
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufRegister(void *devPtr, size_t size);

/*!
 * @brief Register a host buffer for RDMA transfers
 *
 * Registers user-owned CPU RAM, such as a buffer allocated
 * with malloc() or hipHostMalloc(), with the RDMA NIC.
 * hipObject does not allocate or free the host buffer.
 * Maximum 4 GiB per registration.
 *
 * @param hostPtr  Pointer to CPU-accessible memory
 * @param size     Size of the buffer in bytes
 * @return hipObjError_t
 * @serialized
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufRegisterHost(void *hostPtr, size_t size);

/*!
 * @brief Deregister a previously registered buffer
 *
 * @param devPtr  Pointer previously passed to
 *                hipObjBufRegister or hipObjBufRegisterHost
 * @return hipObjError_t
 * @serialized
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufDeregister(void *devPtr);

/* -------------------------------------------------------
 *  DATA TRANSFER
 * ------------------------------------------------------- */

/*!
 * @brief GET: fetch an S3 object into a registered
 *        buffer via RDMA
 *
 * The server performs an RDMA WRITE to push data into
 * the registered buffer, starting @p offset bytes into
 * it. The bytes from offset to offset + size must lie
 * inside the buffer. hipObject doesn't send an object
 * offset; the request the callbacks make selects the
 * part of the object to fetch.
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the registered buffer
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @serialized
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGet(hipObjHandle_t handle, void *devPtr, size_t size, off_t offset,
                                   hipObjOps_t *ops, void *ctx);

/*!
 * @brief PUT: store a registered buffer to an S3 object
 *        via RDMA
 *
 * The server performs an RDMA READ to pull data from
 * the registered buffer, starting @p offset bytes into
 * it. The bytes from offset to offset + size must lie
 * inside the buffer. hipObject doesn't send an object
 * offset; the request the callbacks make selects where
 * the data goes in the object.
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the registered buffer
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @serialized
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPut(hipObjHandle_t handle, const void *devPtr, size_t size, off_t offset,
                                   hipObjOps_t *ops, void *ctx);

/* -------------------------------------------------------
 *  hipobj-rc-v2 (TWO-ROUND-TRIP CONTROL PROTOCOL)
 * ------------------------------------------------------- */

#ifdef HIPOBJECT_V2_API

/*!
 * @brief V2 control endpoint settings
 * @ingroup v2
 *
 * The v2 protocol runs its prepare/ready/cancel exchange on a dedicated
 * control endpoint; there is no default port, the caller must supply one.
 */
typedef struct {
    const char *controlEndpoint; /*!< Required "http(s)://host:port" URI;
                                      the library copies the string */
} hipObjControlEndpointV2_t;

/*!
 * @brief V2 initialization configuration
 * @ingroup v2
 */
typedef struct {
    hipObjConfig_t            v1;      /*!< All v1 fields */
    hipObjControlEndpointV2_t control; /*!< v2 control endpoint (required) */
    uint32_t connectDeadlineMs;        /*!< 0 = default (10 s). Bounds the
                                            local RTR/RTS transition phase.
                                            This protocol has no separately
                                            waitable RDMA-connect handshake:
                                            the remote endpoint is learned
                                            from PREPARE and the transitions
                                            are local verbs calls, so the
                                            budget bounds that phase and is
                                            always capped by the transfer
                                            deadline. */
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
 * @ingroup v2
 *
 * Borrow contract: string fields are owned by the library and remain
 * valid for the duration of a single callback invocation. Callbacks are
 * synchronous (they return before the library continues) and must copy
 * anything they need to keep.
 */
typedef struct {
    const char                      *method;    /*!< "GET" or "PUT" */
    const char                      *bucket;    /*!< Object bucket */
    const char                      *key;       /*!< Object key */
    const char                      *query;     /*!< Canonical query string or NULL */
    const char                      *token;     /*!< 88-hex token[:addr:size] */
    const char                      *session;   /*!< READY/cancel: session id (library sets) */
    const char                      *target;    /*!< Canonical rdma-target value (library sets) */
    uint64_t                         size;      /*!< Transfer size in bytes */
    uint64_t                         offset;    /*!< Byte offset into the object */
    uint32_t                         cookie;    /*!< Client cookie (library generates) */
    uint32_t                         clientPsn; /*!< Client PSN, 1..0xffffff (library generates) */
    uint32_t                         deadlineMs;  /*!< Whole-transfer budget remaining, set by the
                                                       library before every callback. Zero means
                                                       the budget is exhausted; callbacks should
                                                       fail fast. */
    uint32_t                         remainingMs; /*!< Budget relevant to the current callback:
                                                       the transfer's remaining budget for in-
                                                       flight phases, or the fresh cleanup budget
                                                       for the post-expiry CANCEL. */
    const hipObjControlEndpointV2_t *endpoint;  /*!< Control endpoint (library sets
                                                   from init) */
    uint32_t                         clientQpn;    /*!< READY: this transfer's QP number.
                                                        Zero before the endpoint exists. */
    uint64_t                         clientMrAddr; /*!< READY: registered buffer address.
                                                        For GET the server WRITEs the object into
                                                        it; for PUT it is bookkeeping only. Zero
                                                        before READY. */
    uint32_t                         clientMrRkey; /*!< READY: registered buffer rkey */
} hipObjTransferReqV2_t;

/*! @brief Response to PREPARE @ingroup v2 */
typedef struct {
    int      httpStatus;        /*!< Status code (200/501/403/413/503/500) */
    int      protocolEcho;      /*!< 1 when X-Amz-Rdma-Protocol: hipobj-rc-v2 seen */
    int      unsupportedMarker; /*!< 1 when the explicit unsupported marker seen */
    char     serverToken[97];   /*!< 88-hex peer token + NUL */
    char     session[65];       /*!< 32-hex session id + NUL */
    uint32_t serverPsn;         /*!< Server PSN, 1..0xffffff (0 = invalid) */
    uint64_t stagingAddr;       /*!< Server staging MR address (PUT), 0 when
                                     absent; required for PUT data delivery */
    uint32_t stagingRkey;       /*!< Server staging MR rkey (PUT), 0 when
                                     absent */
    int      stagingPresent;    /*!< 1 when both staging fields were present
                                     and valid; a PUT without a valid staging
                                     advertisement fails the transfer */
} hipObjPrepareReplyV2_t;

/*! @brief FINAL response (the reply to READY) @ingroup v2 */
typedef struct {
    int      httpStatus;      /*!< 200 (GET; PUT accepts 200 or 204) / 5xx / 409 / 408 */
    int      protocolEcho;    /*!< 1 when the protocol echo header was present */
    uint64_t bytes;           /*!< Bytes transferred per the server */
    uint32_t cookieEcho;      /*!< Must match the request cookie */
    char     etag[128];       /*!< S3 ETag when present, else empty */
    char     versionId[128];  /*!< S3 version id when present, else empty */
    char     checksumB64[13]; /*!< 12-char canonical CRC64NVME base64 or empty */
    int      cookiePresent;   /*!< 1 when the cookie echo header was present */
} hipObjFinalReplyV2_t;

/*!
 * @brief Phase-aware callbacks for the v2 control protocol
 * @ingroup v2
 *
 * sendPrepare and sendCancel each perform one complete HTTP round trip
 * on the control endpoint. The READY exchange is split: sendReadyRequest
 * writes the request bytes and returns, the data phase runs while the
 * exchange is pending, then finishReady reads the response. All callbacks
 * are required for v2 transfers. The v1 member is unused by the v2 entry
 * points and is kept for structural forward compatibility.
 *
 * The callbacks run while hipObject holds its API lock, so they can't
 * call serialized functions; see @ref threads.
 */
typedef struct hipObjOpsV2 {
    hipObjOps_t v1;

    /*! Issue PREPARE; out is filled from the response headers. */
    int (*sendPrepare)(void *ctx, const hipObjTransferReqV2_t *req, hipObjPrepareReplyV2_t *out);

    /*! Issue the READY request: write the request bytes and return once
     * they are flushed. Does not read the response. Success means the
     * complete request was handed to the transport; a partial write or I/O
     * error is a failure, and finishReady is then never called for it. */
    int (*sendReadyRequest)(void *ctx, const hipObjTransferReqV2_t *req);

    /*! Read the READY response (FINAL) and fill out. Called at most once,
     * and exactly once when sendReadyRequest succeeded and the exchange
     * reaches a terminal disposition. On a data-phase failure or budget
     * expiry the consumer aborts the connection instead; an aborted
     * connection must never be reused or returned to a pool. */
    int (*finishReady)(void *ctx, const hipObjTransferReqV2_t *req, hipObjFinalReplyV2_t *out);

    /*! Issue CANCEL (idempotent). Only the HTTP status matters. */
    int (*sendCancel)(void *ctx, const hipObjTransferReqV2_t *req);
} hipObjOpsV2_t;

/*!
 * @brief Initialize the library for hipobj-rc-v2 transfers
 * @ingroup v2
 *
 * Mutually exclusive with hipObjInit: whichever is called first wins and
 * the other returns hipObjAlreadyInitialized until hipObjShutdown.
 *
 * @serialized
 */
HIPOBJ_API hipObjError_t hipObjInitV2(hipObjConfigV2_t *config);

/*!
 * @brief Query the RDMA NIC name selected by hipObjInitV2
 * @ingroup init
 *
 * Returns an owned copy of the device name chosen at init (the config
 * hint when one was given), or NULL before hipObjInitV2 has run and
 * when another call already holds the API lock. The snapshot is taken
 * under that lock, so hipObjShutdown cannot free it while it is read.
 * Release with hipObjFreeNicV2. This is the supported way for v2
 * consumers to learn the NIC; no v1 RDMA token is needed.
 */
HIPOBJ_API char* hipObjNicV2(void);

/*! @brief Release a NIC name snapshot from hipObjNicV2 @ingroup init */
HIPOBJ_API void hipObjFreeNicV2(char* nic);

/*!
 * @brief V2 GET: download an object into a registered buffer
 * @ingroup v2
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
 * @serialized
 */
HIPOBJ_API hipObjError_t hipObjGetV2(const char *bucket, const char *key, void *devPtr, uint64_t size,
                                     uint64_t offset, const char *query, hipObjOpsV2_t *ops, void *ctx);

/*!
 * @brief V2 PUT, same contract as hipObjGetV2
 * @serialized
 * @ingroup v2
 */
HIPOBJ_API hipObjError_t hipObjPutV2(const char *bucket, const char *key, const void *devPtr, uint64_t size,
                                     uint64_t offset, const char *query, hipObjOpsV2_t *ops, void *ctx);

#endif /* HIPOBJECT_V2_API */

/*! @brief hipObjBufSync direction: device -> staging buffer @ingroup io */
#define HIPOBJ_SYNC_TO_HOST 0
/*! @brief hipObjBufSync direction: staging buffer -> device @ingroup io */
#define HIPOBJ_SYNC_TO_DEVICE 1

/*!
 * @brief Stage a registered buffer between device and host memory
 *
 * When a device pointer cannot be registered with the NIC directly (no
 * dmabuf support, or a fabric with no peer-to-peer path to the GPU),
 * hipObjBufRegister() registers a host staging buffer instead and the
 * peer's RDMA reads and writes land there. Callers driving a transfer
 * through hipObjGetRdmaToken() must therefore call this with
 * HIPOBJ_SYNC_TO_HOST before a PUT and HIPOBJ_SYNC_TO_DEVICE after a
 * GET. It is a no-op, reporting success, for a directly registered
 * buffer; hipObjGet()/hipObjPut() and the V2 entry points do it
 * themselves.
 *
 * @serialized
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjBufSync(void *devPtr, size_t size, off_t offset, int direction);

/*!
 * @brief Mint a hex-encoded RC RDMA token for a registered buffer
 *
 * The caller must release @p *outToken with hipObjPutRdmaToken().
 * @p op is HIPOBJ_RDMA_OP_PUT or HIPOBJ_RDMA_OP_GET (reserved).
 *
 * @serialized
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGetRdmaToken(const void *devPtr, size_t size, int op, char **outToken);

/*!
 * @brief Release a token allocated by hipObjGetRdmaToken()
 * @concurrent
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPutRdmaToken(char *token);

/*!
 * @brief Parse an x-amz-rdma-reply header value
 *
 * On success writes the HTTP-style reply code (200, 204, 206, 501)
 * to @p httpCode.
 *
 * @concurrent
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjParseRdmaReply(const char *reply, size_t replyLen, int *httpCode);

/*!
 * @brief Extract client NIC IPv4 from a minted RDMA token
 *
 * Writes a dotted-quad address into @p nicIp when the token GID
 * carries an IPv4-mapped RoCEv2 suffix.  Returns hipObjSuccess with
 * an empty string when no address is encoded.
 *
 * @concurrent
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjTokenClientNic(const char *token, char *nicIp, size_t nicIpLen);

/*!
 * @brief Return the library version as a string
 * @concurrent
 * @ingroup core
 */
HIPOBJ_API const char *hipObjGetVersionString(void);

#ifdef __cplusplus
}
#endif
