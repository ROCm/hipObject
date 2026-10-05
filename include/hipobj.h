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
 * Every hipObject function can be called from any thread at any time,
 * including from the hipObjOps_t callbacks, and calls on different
 * threads run at the same time. A call waits for another only briefly,
 * while the other changes something that both use, such as the list of
 * registered buffers, with two exceptions: a transfer can wait for an
 * RDMA connection (see below), and calls to hipObjInit() and
 * hipObjShutdown() run one at a time. hipObjInit() reads hipObject's
 * environment variables, so it mustn't run while another thread changes
 * the environment, for example with setenv(), putenv(), or unsetenv().
 *
 * **Transfers.** hipObjGet() and hipObjPut() calls run at the same time
 * as each other, and as every other call. Each transfer uses an RDMA
 * connection of its own, from a set that hipObjInit() creates. The
 * HIPOBJ_MAX_TRANSFERS environment variable sets how many it creates,
 * and so how many transfers can be in progress at once: a whole number
 * from 1 to 1024. A value that isn't one is ignored, leaving the default
 * of 16. A transfer that starts while every connection is in use waits
 * for one to be free.
 *
 * **Callbacks.** A callback can call any function, including hipObjGet()
 * and hipObjPut(). A transfer that a callback starts doesn't wait for a
 * connection, since the one it would wait for could be the one the
 * callback's own transfer holds: if none is free, it returns hipObjBusy.
 * A callback that waits for another thread's transfer can wait forever
 * for the same reason, and hipObject can't detect it. hipObject tells
 * which transfers a callback starts by the thread it runs on, so a
 * callback must return on the thread that called it. This matters if it
 * runs in a fiber or coroutine that can move to another thread.
 *
 * **Buffers.** hipObjBufDeregister() returns hipObjBusy, and leaves the
 * buffer registered, while another call is using the buffer: a transfer,
 * including the one whose callback deregisters it, hipObjBufSync(), or
 * hipObjGetRdmaToken(). Calls on other buffers don't wait for each
 * other. As with any C function, calls that run at the same time mustn't
 * share memory that one of them writes to: overlapping parts of a
 * registered buffer, or a token that one of them releases. For a buffer
 * that's staged through host memory (see hipObjBufSync()), hipObjPut()
 * and HIPOBJ_SYNC_TO_HOST write the staging buffer, so they count as
 * writing the part of the buffer they use.
 *
 * **Shutdown.** hipObjShutdown() doesn't wait for the calls in progress.
 * They finish normally, and hipObject releases its RDMA resources and
 * buffer registrations when the last of them returns. A transfer that's
 * waiting for a connection returns hipObjNotInitialized. Calls that start
 * after hipObjShutdown(), on any thread, and need an initialized library
 * fail with hipObjNotInitialized until hipObjInit() is called again,
 * which can be before the earlier calls have finished.
 *
 * **Transfers made of several calls.** A transfer that the application
 * drives itself, with hipObjGetRdmaToken(), its own S3 request, and
 * hipObjBufSync(), is several calls, and hipObject doesn't know that the
 * transfer is in progress between them. Other threads' calls can run
 * between them, and nothing keeps those from deregistering the buffer or
 * shutting hipObject down: the application must keep the buffer
 * registered, and hipObject initialized, until the transfer ends. Every
 * token from hipObjGetRdmaToken() names the same RDMA connection, so the
 * application must also make sure that such transfers don't overlap
 * each other. They can overlap hipObjGet() and hipObjPut() transfers,
 * which use connections of their own.
 */

/* -------------------------------------------------------
 *  LIBRARY VERSION NUMBERS
 * ------------------------------------------------------- */

/* This is the only place the version is set. CMakeLists.txt and
 * docs/conf.py read it from these lines, so keep each one in the form
 * #define HIPOBJ_VERSION_<part> <number>. */

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
    /*!
     * The buffer or connection is in use. A staging copy of the buffer
     * that timed out is still running (see hipObjBufSync()); another
     * call is using a buffer that hipObjBufDeregister() was given; or a
     * transfer started from a callback found every RDMA connection in
     * use (see @ref threads). Try again later. hipObject added this
     * code; cuObject doesn't have it.
     */
    hipObjBusy,
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
 * The callbacks can call any hipObject function; see
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
 *
 * hipObject doesn't make S3 requests, so it doesn't use endpoint,
 * region, accessKey, or secretKey: the application's S3 client does.
 *
 * gpuDevice is the HIP device index of the GPU that hipObject chooses
 * the closest RDMA NIC to, or -1 for the calling thread's current
 * device. On a host without a GPU, set it to -1, and set nicHint.
 *
 * nicHint lists the RDMA devices that hipObject can choose from, by
 * name (as ibv_devices lists them), separated by commas; NULL or ""
 * allows them all. If hipObject can't choose a NIC by where the GPU
 * is, as on a host without one, it opens the device that nicHint
 * names, which must then be a single name.
 *
 * Set the fields you don't use to 0 or NULL.
 *
 * @ingroup core
 */
typedef struct {
    const char *endpoint;  /*!< Not used                  */
    const char *region;    /*!< Not used                  */
    const char *accessKey; /*!< Not used                  */
    const char *secretKey; /*!< Not used                  */
    uint32_t    flags;     /*!< Reserved, set to 0        */
    int         gpuDevice; /*!< HIP device index, or -1   */
    const char *nicHint;   /*!< RDMA NIC names, or NULL   */
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
 * Selects the closest NIC to the GPU (see
 * hipObjConfig_t), opens it, and creates the RDMA
 * connections that transfers use (see @ref threads).
 *
 * hipObjGetErrorString(), hipObjGetVersionString(),
 * hipObjGetVersion(), hipObjParseRdmaReply(),
 * hipObjTokenClientNic(), and
 * hipObjPutRdmaToken() work without it, and
 * hipObjShutdown() does nothing without it. The other
 * functions return hipObjNotInitialized until it has
 * succeeded.
 *
 * It reads hipObject's environment variables, so
 * don't change the environment on another thread
 * while it runs (see @ref threads).
 *
 * Returns hipObjInvalidValue if config is NULL, flags
 * isn't 0, or gpuDevice is below -1 or names a GPU that
 * doesn't exist; hipObjAlreadyInitialized if hipObject
 * is already initialized; hipObjNicNotFound if there's
 * no NIC to use; and hipObjRdmaError if libibverbs
 * doesn't load, a HIP call fails (hipError says which
 * error), or the NIC can't be opened or the connections
 * created.
 *
 * @param config  Pointer to configuration struct
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjInit(hipObjConfig_t *config);

/*!
 * @brief Shut down the hipObject library
 *
 * Releases all RDMA resources.  All registered
 * buffers are implicitly deregistered.
 *
 * It doesn't wait for calls in progress on other
 * threads, or for the transfer whose callback calls
 * it. They finish normally, and the resources they
 * use are released when the last of them returns
 * (see @ref threads).
 *
 * A staging buffer that a copy that timed out may
 * still use (see hipObjBufSync()) isn't freed.
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
 * The buffer must lie within a single HIP allocation:
 * returns hipObjInvalidValue if it runs past the end
 * of the allocation that holds devPtr.
 *
 * @param devPtr  Pointer returned by hipMalloc
 * @param size    Size of the buffer in bytes
 * @return hipObjError_t
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
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufRegisterHost(void *hostPtr, size_t size);

/*!
 * @brief Deregister a previously registered buffer
 *
 * While another call is using the buffer (see
 * @ref threads), or a staging copy of the buffer
 * that timed out is still running (see
 * hipObjBufSync()), this returns hipObjBusy and
 * leaves the buffer registered. Call it again once
 * the call or the copy has finished, and don't free
 * the buffer until then.
 *
 * @param devPtr  Pointer previously passed to
 *                hipObjBufRegister or hipObjBufRegisterHost
 * @return hipObjError_t
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
 * If every RDMA connection is in use by other
 * transfers, this waits for one to be free, or, if
 * it's called from a callback, returns hipObjBusy
 * (see @ref threads).
 *
 * For a buffer that's staged through host memory (see
 * hipObjBufSync()), if the copy into the buffer doesn't
 * finish within the staging timeout (the
 * HIPOBJ_STAGE_TIMEOUT_MS environment variable), this
 * returns hipObjInternalError and leaves the copy
 * running. It can still write the buffer after this
 * returns, and until it finishes, calls that start
 * after this returns can't use or deregister the
 * buffer (see hipObjBufSync()).
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the registered buffer
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
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
 * If every RDMA connection is in use by other
 * transfers, this waits for one to be free, or, if
 * it's called from a callback, returns hipObjBusy
 * (see @ref threads).
 *
 * For a buffer that's staged through host memory (see
 * hipObjBufSync()), if the copy out of the buffer
 * doesn't finish within the staging timeout (the
 * HIPOBJ_STAGE_TIMEOUT_MS environment variable), this
 * returns hipObjInternalError and leaves the copy
 * running. It can still read the buffer after this
 * returns, and until it finishes, calls that start
 * after this returns can't use or deregister the
 * buffer (see hipObjBufSync()).
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the registered buffer
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
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
 * Until it finishes, hipObject keeps the staging buffer it uses, and
 * calls that start after the copy times out can't use the buffer:
 * hipObjGet(), hipObjPut(), hipObjBufSync(), and hipObjGetRdmaToken()
 * return hipObjBusy for it, and hipObjBufDeregister() returns hipObjBusy
 * and leaves it registered. Calls that had already started aren't
 * stopped.
 * Once the copy has finished, the buffer can be used and deregistered
 * again. If it never finishes, the staging buffer is never freed.
 *
 * hipObject doesn't wait for GPU work that uses the buffer, on any
 * stream. Before HIPOBJ_SYNC_TO_HOST,
 * make sure that the GPU work that writes the buffer has finished, for
 * example with hipStreamSynchronize(). Before HIPOBJ_SYNC_TO_DEVICE, make
 * sure that no GPU work that reads or writes the buffer is still running.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjBufSync(void *devPtr, size_t size, off_t offset, int direction);

/*!
 * @brief Mint a hipobj-rc-v3 token for a registered buffer
 *
 * Returns the same string hipObjPut()/hipObjGet() send as the
 * x-amz-rdma-token header value: the RC-CM listener's address plus
 * the buffer's rkey/remoteAddr/length. It carries no QP number, GID
 * or LID -- the peer dials the listener and RDMA CM negotiates the
 * connection itself (see rc-listener.h, v3-wire.h).
 *
 * The caller must release @p *outToken with hipObjPutRdmaToken().
 * @p op is HIPOBJ_RDMA_OP_PUT or HIPOBJ_RDMA_OP_GET (reserved).
 *
 * Returns hipObjBusy while a staging copy of the buffer that timed out
 * is still running (see hipObjBufSync()).
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGetRdmaToken(const void *devPtr, size_t size, int op, char **outToken);

/*!
 * @brief Release a token allocated by hipObjGetRdmaToken()
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPutRdmaToken(char *token);

/*!
 * @brief Parse an x-amz-rdma-reply header value
 *
 * On success writes the HTTP-style reply code (200, 204, 206, 501)
 * to @p httpCode.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjParseRdmaReply(const char *reply, size_t replyLen, int *httpCode);

/*!
 * @brief Parse an x-amz-rc-rdma-reply header value (hipobj-rc-v3 only)
 *
 * Distinct from hipObjParseRdmaReply(): the v3 reply format is
 * "<httpStatus>:<bytesTransferred-hex16>" (see v3-wire.h), not the bare
 * status code v1/v2 use, so it also returns @p bytesTransferred. Use
 * this for tokens minted for the RC-CM listener path
 * (HIPOBJ_RC_LISTEN_IP / hipObjGetRdmaToken() against that listener),
 * hipObjParseRdmaReply() for v1/v2.
 *
 * @concurrent
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjParseRdmaReplyV3(const char* reply,
                                                size_t replyLen, int* httpCode,
                                                uint64_t* bytesTransferred);

/*!
 * @brief Extract client NIC IPv4 from a minted RDMA token
 *
 * Writes a dotted-quad address into @p nicIp when the token GID
 * carries an IPv4-mapped RoCEv2 suffix.  Returns hipObjSuccess with
 * an empty string when no address is encoded.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjTokenClientNic(const char *token, char *nicIp, size_t nicIpLen);

/*!
 * @brief Return the library version as a string
 * @ingroup core
 */
HIPOBJ_API const char *hipObjGetVersionString(void);

/*!
 * @brief Get the library's version as numbers
 *
 * Writes the major, minor, and patch version of the hipObject library
 * that's running. These can differ from the HIPOBJ_VERSION_MAJOR,
 * HIPOBJ_VERSION_MINOR, and HIPOBJ_VERSION_PATCH macros of the header
 * that the application was compiled with, if it's linked against a
 * different hipObject.
 *
 * A NULL pointer skips that part of the version, and the others are
 * still written. It always returns hipObjSuccess.
 *
 * @param major  Where to write the major version, or NULL
 * @param minor  Where to write the minor version, or NULL
 * @param patch  Where to write the patch version, or NULL
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjGetVersion(unsigned *major, unsigned *minor, unsigned *patch);

#ifdef __cplusplus
}
#endif
