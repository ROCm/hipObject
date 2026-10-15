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
 * Make sure that no GPU work that reads or writes the
 * buffer is still running when you call this. When it
 * returns hipObjSuccess, the data is in the buffer.
 *
 * For a buffer that's staged through host memory (see
 * hipObjBufSync()), if the copy into the buffer doesn't
 * finish within the staging timeout (the
 * HIPOBJ_STAGE_TIMEOUT_MS environment variable), this
 * returns hipObjInternalError and leaves the copy
 * running. It can still write the buffer after this
 * returns.
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
 * hipObject doesn't wait for GPU work that writes the
 * buffer, on any stream. Make sure that it has finished,
 * for example with hipStreamSynchronize(), before you
 * call this.
 *
 * For a buffer that's staged through host memory (see
 * hipObjBufSync()), if the copy out of the buffer
 * doesn't finish within the staging timeout (the
 * HIPOBJ_STAGE_TIMEOUT_MS environment variable), this
 * returns hipObjInternalError and leaves the copy
 * running. It can still read the buffer after this
 * returns.
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
 * buffer; hipObjGet() and hipObjPut() do it themselves.
 *
 * When this returns hipObjSuccess, the copy has finished. If the copy
 * doesn't finish within the staging timeout (the HIPOBJ_STAGE_TIMEOUT_MS
 * environment variable), this returns hipObjInternalError and leaves the
 * copy running. It can still read or write the buffer after this returns.
 *
 * hipObject doesn't wait for GPU work that uses the buffer, on any
 * stream. Before HIPOBJ_SYNC_TO_HOST,
 * make sure that the GPU work that writes the buffer has finished, for
 * example with hipStreamSynchronize(). Before HIPOBJ_SYNC_TO_DEVICE, make
 * sure that no GPU work that reads or writes the buffer is still running.
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
