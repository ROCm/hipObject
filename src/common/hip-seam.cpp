/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hip-seam.h"

#include <cstdio>
#include <optional>

#include <hsa/hsa_ext_amd.h>

#include "hipobj-print.h"

namespace hipObj {

hipError_t
SystemHip::getDevice(int *device) const
{
    return hipGetDevice(device);
}

hipError_t
SystemHip::setDevice(int device) const
{
    return hipSetDevice(device);
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
SystemHip::exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const
{
    return static_cast<int>(hsa_amd_portable_export_dmabuf(ptr, size, fd, offset));
}

int
SystemHip::closeDmabuf(int fd) const
{
    return static_cast<int>(hsa_amd_portable_close_dmabuf(fd));
}

DeviceGuard::DeviceGuard(const HipRuntime &runtime, std::optional<int> device) : runtime_(&runtime)
{
    if (!device) {
        return;
    }
    /* hipGetDevice() only reads the thread's own setting, so the usual
     * case, where the device is already current, costs very little */
    status_ = runtime.getDevice(&previous_);
    if (status_ != hipSuccess || previous_ == *device) {
        return;
    }
    status_   = runtime.setDevice(*device);
    switched_ = status_ == hipSuccess;
}

DeviceGuard::~DeviceGuard()
{
    if (!switched_) {
        return;
    }
    /* A device that was current a moment ago should always be accepted,
     * but if it isn't, the caller's thread is left on another device, so
     * say so */
    const hipError_t err = runtime_->setDevice(previous_);
    if (err != hipSuccess) {
        printTo(stderr, "hipObj: couldn't make HIP device {} current again: HIP error {}\n", previous_,
                static_cast<int>(err));
    }
}

} // namespace hipObj
