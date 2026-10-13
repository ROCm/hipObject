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
 */

/*!
 * @defgroup io Data Transfer (GET / PUT)
 *
 * hipObjGet() and hipObjPut() use the cuObject-compatible protocol, which
 * exchanges RDMA tokens in headers of the application's own S3 request
 * and its response. The server moves the data with RDMA, writing the
 * buffer for a GET and reading it for a PUT. On the client, hipObject:
 *
 * 1. passes an RDMA token that describes the buffer to the hipObjOps_t
 *    sendRequest callback, which sends it in the request's
 *    x-amz-rdma-token header;
 * 2. gets the response's x-amz-rdma-reply header from the recvReply
 *    callback;
 * 3. connects its queue pair to the server's token, when the reply
 *    carries one; and
 * 4. waits for the transfer's completion.
 *
 * The application's S3 client chooses the object, the range of it, and
 * the endpoint; hipObject only sees the buffer.
 *
 * Every transfer uses RDMA Reliable Connection (RC) transport, over the
 * one queue pair that hipObjInit() creates. hipObject doesn't implement
 * Dynamic Connection (DC) transport. A token's transport byte has a DC
 * value only because the token format comes from cuObject, whose servers
 * use DC on NVIDIA ConnectX NICs; an RC-to-DC adapter on the server
 * bridges the two (see the interoperability guide, docs/interop.rst).
 *
 * hipObjGetRdmaToken(), hipObjBufSync(), hipObjParseRdmaReply(), and
 * hipObjTokenClientNic() are for applications that drive the exchange
 * themselves instead of through callbacks.
 *
 * The experimental V2 API (@ref v2) is a different control protocol over
 * the same RC transport, not a newer version of these functions.
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
 * @defgroup v2 Experimental V2 Control Protocol (hipobj-rc-v2)
 *
 * @if HIPOBJ_HEADER_ONLY
 * @warning The V2 API is experimental. It may change incompatibly or be
 * removed in any release. hipObjGetV2() and hipObjPutV2() are not
 * implemented yet and return hipObjNotSupported.
 * @endif
 *
 * The V2 API runs a transfer with a different control protocol,
 * hipobj-rc-v2. It isn't a newer version of hipObjGet() and hipObjPut(),
 * and it isn't what makes transfers use RC: both APIs move the data with
 * RDMA over RC (see @ref io). The "rc" in the protocol's name is the
 * transport it sets up, not a difference from the other API. What
 * differs is how the client and the server agree on a transfer:
 *
 * - **Control channel.** hipObjGet() and hipObjPut() exchange headers in
 *   the application's S3 request and its response, in one round trip.
 *   hipobj-rc-v2 is hipObject's own protocol, which cuObject servers
 *   don't speak. It runs in two round trips on a dedicated control
 *   endpoint, separate from the S3 endpoint. PREPARE exchanges the two
 *   sides' tokens and starting packet sequence numbers and opens a
 *   session, and the response to READY, called FINAL, reports the outcome
 *   of the transfer: the bytes transferred, and the object's ETag, version
 *   ID, and checksum. CANCEL abandons a prepared transfer.
 * - **Object and offset.** The V2 functions name the object (bucket, key,
 *   and an optional query string), and their offset is a byte offset into
 *   the object. hipObjGet() and hipObjPut() leave the object to the
 *   application's request, and their offset is into the buffer.
 * - **Connection.** A V2 transfer is designed to use its own queue pair,
 *   for a session that ends when the call returns, instead of the one
 *   that hipObjInit() creates for hipObjGet() and hipObjPut().
 *
 * The V2 API can't transfer data yet. hipObjGetV2() and hipObjPutV2()
 * check their arguments and then return hipObjNotSupported, the error
 * that also means a server doesn't support hipobj-rc-v2, so an
 * application falls back to hipObjGet() and hipObjPut() in either case.
 * There is also no way to give the library the control endpoint:
 * hipObjInitV2() isn't available, so no function accepts a
 * hipObjConfigV2_t or a hipObjControlEndpointV2_t.
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
    hipObjNotSupported, /*!< V2 API: server explicitly does not support hipobj-rc-v2, or the
                             V2 function isn't implemented yet */
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
 * The transfer uses the cuObject-compatible protocol
 * over RC; see @ref io.
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
 * The transfer uses the cuObject-compatible protocol
 * over RC; see @ref io.
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
 *
 * No function accepts this yet; see hipObjConfigV2_t.
 */
typedef struct {
    const char *controlEndpoint; /*!< Required "http(s)://host:port" URI;
                                      the library copies the string */
} hipObjControlEndpointV2_t;

/*!
 * @brief V2 initialization configuration
 * @ingroup v2
 *
 * This is the configuration for hipObjInitV2(), which isn't available
 * yet, so no function accepts it.
 */
typedef struct {
    hipObjConfig_t            v1;      /*!< All v1 fields */
    hipObjControlEndpointV2_t control; /*!< v2 control endpoint (required) */
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
    const char                      *token;     /*!< 88-hex RDMA token */
    const char                      *session;   /*!< READY/cancel: session id (library sets) */
    const char                      *target;    /*!< Canonical rdma-target value (library sets) */
    uint64_t                         size;      /*!< Transfer size in bytes */
    uint64_t                         offset;    /*!< Byte offset into the object, not the buffer */
    uint32_t                         cookie;    /*!< Client cookie (library generates) */
    uint32_t                         clientPsn; /*!< Client PSN, 1..0xffffff (library generates) */
    const hipObjControlEndpointV2_t *endpoint;  /*!< Control endpoint (library sets
                                                   from init) */
} hipObjTransferReqV2_t;

/*! @brief Response to PREPARE @ingroup v2 */
typedef struct {
    int      httpStatus;        /*!< Status code (200/501/403/413/503/500) */
    int      protocolEcho;      /*!< 1 when X-Amz-Rdma-Protocol: hipobj-rc-v2 seen */
    int      unsupportedMarker; /*!< 1 when the explicit unsupported marker seen */
    char     serverToken[97];   /*!< 88-hex peer token + NUL */
    char     session[65];       /*!< 32-hex session id + NUL */
    uint32_t serverPsn;         /*!< Server PSN, 1..0xffffff (0 = invalid) */
} hipObjPrepareReplyV2_t;

/*! @brief FINAL response (the reply to READY) @ingroup v2 */
typedef struct {
    int      httpStatus;      /*!< 200 (GET) / 204 (PUT) / 5xx / 409 / 408 */
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
 * Each send* callback performs one complete HTTP round trip on the
 * control endpoint and fills @p out from the response. All callbacks are
 * required for v2 transfers. The v1 member is unused by the v2 entry
 * points and is kept for structural forward compatibility.
 *
 * The callbacks run while hipObject holds its API lock, so they can't
 * call serialized functions; see @ref threads.
 */
typedef struct hipObjOpsV2 {
    hipObjOps_t v1; /*!< Unused by hipObjGetV2() and hipObjPutV2() */

    /*! Issue PREPARE; out is filled from the response headers. */
    int (*sendPrepare)(void *ctx, const hipObjTransferReqV2_t *req, hipObjPrepareReplyV2_t *out);

    /*! Issue READY; the response is FINAL. out reflects it. */
    int (*sendReady)(void *ctx, const hipObjTransferReqV2_t *req, hipObjFinalReplyV2_t *out);

    /*! Issue CANCEL (idempotent). Only the HTTP status matters. */
    int (*sendCancel)(void *ctx, const hipObjTransferReqV2_t *req);
} hipObjOpsV2_t;

/* hipObjInitV2() isn't implemented, so its declaration is disabled.
 * Callers get a compile error instead of an undefined symbol at link time.
 */
#if 0
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
#endif

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
 * Unlike hipObjGet(), this function names the object itself, and
 * @p offset is a byte offset into the object, not into the buffer. The
 * data lands at the start of @p devPtr.
 *
 * Not implemented yet: after checking its arguments, it returns
 * hipObjNotSupported.
 *
 * @param bucket  Bucket name, 1 to 255 bytes
 * @param key     Object key, 1 to 1024 bytes
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer, at most 2^31-1
 * @param offset  Byte offset into the object
 * @param query   Canonical S3 query string, or NULL or "" for none:
 *                key=value parameters separated by '&', sorted by key
 *                and then by value, with the '=' present even when the
 *                value is empty. In each key and value, the RFC 3986
 *                unreserved characters are written as is and every
 *                other character is percent-encoded with uppercase hex
 *                digits.
 * @param ops     Phase callbacks; all three send* callbacks are required
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @serialized
 */
HIPOBJ_API hipObjError_t hipObjGetV2(const char *bucket, const char *key, void *devPtr, uint64_t size,
                                     uint64_t offset, const char *query, hipObjOpsV2_t *ops, void *ctx);

/*!
 * @brief V2 PUT, same contract as hipObjGetV2
 *
 * As with hipObjGetV2(), @p offset is a byte offset into the object, not
 * into the buffer, and the data comes from the start of @p devPtr.
 *
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
 * The token covers the first @p size bytes of the buffer, and is what
 * hipObjGet() and hipObjPut() send in the x-amz-rdma-token header. The
 * caller must release @p *outToken with hipObjPutRdmaToken(). "Get" and
 * "Put" in these two names mean create and release, not S3 GET and PUT.
 * @p op must be HIPOBJ_RDMA_OP_PUT or HIPOBJ_RDMA_OP_GET. It's reserved:
 * the token is the same for either.
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
