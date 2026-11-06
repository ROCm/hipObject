/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hip-seam.h"

#ifndef HIPOBJ_CPU_RDMA
#include <hsa/hsa_ext_amd.h>
#endif

namespace hipObj {

hipError_t
SystemHip::getDevice(int *device) const
{
    return hipGetDevice(device);
}

hipError_t
SystemHip::getDeviceCount(int *count) const
{
    return hipGetDeviceCount(count);
}

hipError_t
SystemHip::deviceGetPCIBusId(char *pciBusId, int len, int device) const
{
    return hipDeviceGetPCIBusId(pciBusId, len, device);
}

hipError_t
SystemHip::hostMalloc(void **ptr, size_t size, unsigned int flags) const
{
    return hipHostMalloc(ptr, size, flags);
}

hipError_t
SystemHip::hostFree(void *ptr) const
{
    return hipHostFree(ptr);
}

hipError_t
SystemHip::deviceSynchronize() const
{
    return hipDeviceSynchronize();
}

hipError_t
SystemHip::memcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind, hipStream_t stream) const
{
    return hipMemcpyAsync(dst, src, size, kind, stream);
}

hipError_t
SystemHip::eventCreate(hipEvent_t *event) const
{
    return hipEventCreate(event);
}

hipError_t
SystemHip::eventRecord(hipEvent_t event, hipStream_t stream) const
{
    return hipEventRecord(event, stream);
}

hipError_t
SystemHip::eventQuery(hipEvent_t event) const
{
    return hipEventQuery(event);
}

hipError_t
SystemHip::eventDestroy(hipEvent_t event) const
{
    return hipEventDestroy(event);
}

hipError_t
SystemHip::pointerGetAttributes(hipPointerAttribute_t *attributes, const void *ptr) const
{
    return hipPointerGetAttributes(attributes, ptr);
}

hipError_t
SystemHip::memGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t ptr) const
{
    return hipMemGetAddressRange(base, size, ptr);
}

int
SystemHip::exportDmabuf([[maybe_unused]] const void *ptr, [[maybe_unused]] size_t size,
                        [[maybe_unused]] int *fd, [[maybe_unused]] uint64_t *offset) const
{
#ifdef HIPOBJ_CPU_RDMA
    // No HSA/ROCm runtime to export a dmabuf fd from on this build, and
    // host memory has no GPU dmabuf to export in the first place --
    // registerDeviceMemory() always falls back to plain ibv_reg_mr.
    return -1;
#else
    return static_cast<int>(hsa_amd_portable_export_dmabuf(ptr, size, fd, offset));
#endif
}

int
SystemHip::closeDmabuf([[maybe_unused]] int fd) const
{
#ifdef HIPOBJ_CPU_RDMA
    /* exportDmabuf() never succeeds, so there's nothing to close */
    return -1;
#else
    return static_cast<int>(hsa_amd_portable_close_dmabuf(fd));
#endif
}

} // namespace hipObj
