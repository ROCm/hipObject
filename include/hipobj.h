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
 * @brief Parse the PREPARE reply and connect the internal RC QP to the peer.
 *
 * Callers that drive the v2 protocol manually (e.g. the minio-cpp bridge) call
 * this after receiving the PREPARE HTTP response.  The function extracts the
 * server's RDMA token from @p reply (format "200:<tokenHex>[:<sessionId>]"),
 * transitions the library's RC QP to RTR/RTS, and writes the session ID into
 * @p sessionIdBuf (NUL-terminated, at most @p sessionIdBufLen bytes).
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjConnectRdmaPeer(const char* reply,
                                               size_t replyLen,
                                               char* sessionIdBuf,
                                               size_t sessionIdBufLen);

/*!
 * @brief Reset the internal RC QP back to INIT after a completed transfer.
 *
 * Must be called by callers that drive the v2 protocol manually (e.g. the
 * minio-cpp bridge) once the READY phase has completed, so the next transfer
 * can start from a clean INIT state (fixes defect 2).
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjResetRdmaQp(void);

/*!
 * @brief Return the library version as a string
 * @ingroup core
 */
HIPOBJ_API const char* hipObjGetVersionString(void);

#ifdef __cplusplus
}
#endif
