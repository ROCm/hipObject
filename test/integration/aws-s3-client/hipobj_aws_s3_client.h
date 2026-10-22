/* SPDX-License-Identifier: MIT
 *
 * A minimal S3 client, built on top of the AWS SDK for C++, that offloads
 * the data path of GetObject/PutObject to hipObject's RDMA GPU<->NIC
 * transfer instead of streaming bytes through the SDK's HTTP body.
 *
 * Only GetObject and PutObject are hooked into hipObject. Every other
 * S3Client operation (bucket management, multipart, ACLs, listing, ...)
 * is untouched and behaves exactly like the stock AWS SDK client, since
 * this class only adds new methods -- it does not override any of the
 * inherited virtuals.
 */

#pragma once

#include <cstddef>
#include <sys/types.h>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/GetObjectResult.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/PutObjectResult.h>

#include "hipobj.h"

namespace hipobj::aws {

// Outcome aliases so callers do not need to reach into Aws::S3::Model.
using PutObjectOutcome = Aws::S3::Model::PutObjectOutcome;
using GetObjectOutcome = Aws::S3::Model::GetObjectOutcome;

// S3 client that inherits Aws::S3::S3Client so it can be dropped in
// anywhere a normal S3Client is used. PutObjectRdma()/GetObjectRdma() are
// the hipObject-hooked entry points; the caller is responsible for
// hipMalloc'ing devPtr and for hipObjInit() having run before the first
// call (see EnsureHipObjInit()) with a GPU device visible to the process.
//
// The client requires ClientConfiguration::endpointOverride to be set to
// a path-style endpoint ("http://host:port"); S3ClientConfiguration's
// useVirtualAddressing is forced off so bucket/key are joined onto that
// endpoint the same way hipObject's own examples do, since the RDMA
// control exchange is not going through a real AWS region.
class HipObjAwsS3Client : public Aws::S3::S3Client {
public:
    explicit HipObjAwsS3Client(
        const Aws::S3::S3ClientConfiguration &clientConfiguration         = Aws::S3::S3ClientConfiguration(),
        std::shared_ptr<Aws::S3::S3EndpointProviderBase> endpointProvider = nullptr);

    HipObjAwsS3Client(
        const Aws::Auth::AWSCredentials      &credentials,
        const Aws::S3::S3ClientConfiguration &clientConfiguration         = Aws::S3::S3ClientConfiguration(),
        std::shared_ptr<Aws::S3::S3EndpointProviderBase> endpointProvider = nullptr);

    /*!
     * PUT: pulls `size` bytes starting at `offset` out of the GPU buffer at
     * `devPtr` via RDMA. `request` supplies Bucket/Key and any other S3
     * fields the caller wants sent, e.g. metadata: the control request is a
     * copy of it. The RDMA control exchange carries no HTTP payload, so a
     * request with a body, a Content-Length or a Content-MD5 is refused.
     * Unless `devPtr` is already registered, it's registered for
     * `offset + size` bytes for the call.
     */
    PutObjectOutcome PutObjectRdma(const Aws::S3::Model::PutObjectRequest &request, const void *devPtr,
                                   size_t size, off_t offset = 0) const;

    /*!
     * GET: symmetric with PutObjectRdma(). RDMA-writes `size` bytes at
     * `offset` into the GPU buffer at `devPtr`. The control request is a copy
     * of `request`, with its range, version and conditions.
     */
    GetObjectOutcome GetObjectRdma(const Aws::S3::Model::GetObjectRequest &request, void *devPtr, size_t size,
                                   off_t offset = 0) const;

    /*!
     * Registers `devPtr`/`capacity` as an RDMA memory region up front (runs
     * hipObjInit() first if it hasn't already), so a later PutObjectRdma()/
     * GetObjectRdma() call against the same `devPtr` -- for any `size` up to
     * `capacity`, from `offset` 0 -- finds it already registered and skips
     * ibv_reg_mr() entirely: PutObjectRdma()/GetObjectRdma() call
     * hipObjBufRegister() internally too, but it looks buffers up by pointer
     * identity (see BufferMap::registerBuffer()), so a second call against
     * the same pointer is a fast already-registered check rather than a real
     * MR registration. Callers that issue many PUT/GET calls against
     * differently-sized slices of one large pre-allocated buffer should call
     * this once up front (see aws_s3_demo.cpp's `--prereg` mode) so the
     * per-call timing they measure reflects only the RDMA data transfer, not
     * one-time memory-registration cost paid on every call otherwise.
     */
    hipObjError_t RegisterBuffer(void *devPtr, size_t capacity) const;

    /*!
     * Releases a registration made by RegisterBuffer(). Shutdown()
     * deregisters every buffer still registered, but call this before
     * freeing the memory if the process keeps using hipObject.
     */
    hipObjError_t DeregisterBuffer(void *devPtr) const;

    /*!
     * Shuts hipObject down, if a client initialized it: deregisters every
     * buffer and stops the RC-CM listener. Call it after the last transfer
     * and before Aws::ShutdownAPI(). It isn't left to static destruction,
     * where it would release hipObject's RDMA resources while the process
     * exits; a process that doesn't call it exits with hipObject still
     * initialized, which is harmless. A later PutObjectRdma()/GetObjectRdma() initializes it
     * again.
     */
    static hipObjError_t Shutdown();

private:
    struct RdmaControlCtx;

    hipObjError_t EnsureHipObjInit() const;
    int           SendControlRequest(RdmaControlCtx &ctx, const char *token, size_t tokenLen) const;

    static int SendRequestThunk(void *ctx, const char *token, size_t tokenLen);
    static int RecvReplyThunk(void *ctx, char *reply, size_t *replyLen);

    Aws::String endpointOverride_;
    Aws::String region_;
};

} // namespace hipobj::aws
