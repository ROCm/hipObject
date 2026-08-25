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
 */
typedef struct {
  hipObjConfig_t v1;                 /*!< All v1 fields */
  hipObjControlEndpointV2_t control; /*!< v2 control endpoint (required) */
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
  const char* method;  /*!< "GET" or "PUT" */
  const char* bucket;  /*!< Object bucket */
  const char* key;     /*!< Object key */
  const char* query;   /*!< Canonical query string or NULL */
  const char* token;   /*!< 88-hex token[:addr:size] */
  const char* session; /*!< READY/cancel: session id (library sets) */
  const char* target;  /*!< Canonical rdma-target value (library sets) */
  uint64_t size;       /*!< Transfer size in bytes */
  uint64_t offset;     /*!< Byte offset into the object */
  uint32_t cookie;     /*!< Client cookie (library generates) */
  uint32_t clientPsn;  /*!< Client PSN, 1..0xffffff (library generates) */
  const hipObjControlEndpointV2_t* endpoint; /*!< Control endpoint (library sets
                                                from init) */
} hipObjTransferReqV2_t;

/*! @brief Response to PREPARE @ingroup io */
typedef struct {
  int httpStatus;        /*!< Status code (200/501/403/413/503/500) */
  int protocolEcho;      /*!< 1 when X-Amz-Rdma-Protocol: hipobj-rc-v2 seen */
  int unsupportedMarker; /*!< 1 when the explicit unsupported marker seen */
  char serverToken[97];  /*!< 88-hex peer token + NUL */
  char session[65];      /*!< 32-hex session id + NUL */
  uint32_t serverPsn;    /*!< Server PSN, 1..0xffffff (0 = invalid) */
} hipObjPrepareReplyV2_t;

/*! @brief FINAL response (the reply to READY) @ingroup io */
typedef struct {
  int httpStatus;       /*!< 200 (GET) / 204 (PUT) / 5xx / 409 / 408 */
  int protocolEcho;     /*!< 1 when the protocol echo header was present */
  uint64_t bytes;       /*!< Bytes transferred per the server */
  uint32_t cookieEcho;  /*!< Must match the request cookie */
  char etag[128];       /*!< S3 ETag when present, else empty */
  char versionId[128];  /*!< S3 version id when present, else empty */
  char checksumB64[13]; /*!< 12-char canonical CRC64NVME base64 or empty */
} hipObjFinalReplyV2_t;

/*!
 * @brief Phase-aware callbacks for the v2 control protocol
 * @ingroup io
 *
 * Each send* callback performs one complete HTTP round trip on the
 * control endpoint and fills @p out from the response. All callbacks are
 * required for v2 transfers. The v1 member is unused by the v2 entry
 * points and is kept for structural forward compatibility.
 */
typedef struct hipObjOpsV2 {
  hipObjOps_t v1;

  /*! Issue PREPARE; out is filled from the response headers. */
  int (*sendPrepare)(void* ctx, const hipObjTransferReqV2_t* req,
                     hipObjPrepareReplyV2_t* out);

  /*! Issue READY; the response is FINAL. out reflects it. */
  int (*sendReady)(void* ctx, const hipObjTransferReqV2_t* req,
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
 */
HIPOBJ_API hipObjError_t hipObjGetV2(const char* bucket, const char* key,
                                     void* devPtr, uint64_t size,
                                     uint64_t offset, const char* query,
                                     hipObjOpsV2_t* ops, void* ctx);

/*! @brief V2 PUT, same contract as hipObjGetV2 @ingroup io */
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
