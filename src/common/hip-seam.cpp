/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hip-seam.h"

#include <hsa/hsa_ext_amd.h>

#include "hipobj-warnings.h"
#include "no-destructor.h"

namespace hipObj {

// Runtime installed by tests; null means the SystemHip is active. The
// library itself never swaps the pointer.
static const HipRuntime *g_hip_override = nullptr;

const HipRuntime &
hip()
{
    if (g_hip_override) {
        return *g_hip_override;
    }

    /* Never destroyed, so it's still there for a global that releases
     * HIP resources at exit. Clang still warns, since NoDestructor's
     * empty destructor isn't trivial. */
    HIPOBJ_WARN_NO_EXIT_DTOR_OFF
    static NoDestructor<SystemHip> system;
    HIPOBJ_WARN_NO_EXIT_DTOR_ON
    return *system;
}

const HipRuntime *
setHipForTest(const HipRuntime *runtime)
{
    const HipRuntime *previous = g_hip_override;
    g_hip_override             = runtime;
    return previous;
}

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
SystemHip::exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const
{
    return static_cast<int>(hsa_amd_portable_export_dmabuf(ptr, size, fd, offset));
}

int
SystemHip::closeDmabuf(int fd) const
{
    return static_cast<int>(hsa_amd_portable_close_dmabuf(fd));
}

} // namespace hipObj
