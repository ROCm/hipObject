/* SPDX-License-Identifier: MIT
 */

/* Drop-in replacement for <hip/hip_runtime_api.h> when HIPOBJ_CPU_RDMA is
 * set: hipObject only touches a handful of HIP runtime entry points on
 * its host-side RDMA paths (see hip-seam.h), and none of them need an
 * actual GPU or ROCm userspace stack to make sense for a plain NIC
 * RDMA target. This backs them with ordinary host memory instead, so
 * the library and its consumers can link and run on a machine that has
 * RDMA-capable NICs (e.g. Mellanox ConnectX) but no AMD GPU/ROCm
 * install at all.
 */

#pragma once

#include <cstdlib>
#include <cstring>

using hipError_t = int;

constexpr hipError_t   hipSuccess               = 0;
constexpr hipError_t   hipErrorInvalidValue     = 1;
constexpr hipError_t   hipErrorMemoryAllocation = 2;
constexpr hipError_t   hipErrorOutOfMemory      = 2; /* real HIP's name for the same value */
constexpr hipError_t   hipErrorNoDevice         = 100;
constexpr hipError_t   hipErrorInvalidDevice    = 101;
constexpr hipError_t   hipErrorNotFound         = 500;
constexpr hipError_t   hipErrorNotReady         = 600;
constexpr hipError_t   hipErrorLaunchFailure    = 719;
constexpr unsigned int hipHostMallocDefault     = 0;

enum hipMemcpyKind {
    hipMemcpyHostToHost     = 0,
    hipMemcpyHostToDevice   = 1,
    hipMemcpyDeviceToHost   = 2,
    hipMemcpyDeviceToDevice = 3,
    hipMemcpyDefault        = 4
};

using hipStream_t = void *;
/* Real HIP's per-thread default stream. Copies here are synchronous host
 * memcpys, so every stream is the same. */
inline hipStream_t const hipStreamPerThread = nullptr;
using hipEvent_t                            = void *;
using hipDeviceptr_t                        = void *;

enum hipMemoryType { hipMemoryTypeHost = 0, hipMemoryTypeDevice = 1, hipMemoryTypeUnified = 3 };

/* Real HIP's hipPointerAttribute_t has many more fields; only .type is
 * read on this build's paths (buffer.cpp's isDevicePointer()). */
struct hipPointerAttribute_t {
    hipMemoryType type = hipMemoryTypeHost;
};

inline hipError_t
hipGetDevice(int *device)
{
    if (device) {
        *device = 0;
    }
    return hipSuccess;
}

inline hipError_t
hipGetDeviceCount(int *count)
{
    if (count) {
        *count = 0;
    }
    return hipErrorNoDevice;
}

inline hipError_t
hipDeviceGetPCIBusId(char *pciBusId, int len, int)
{
    if (pciBusId && len > 0) {
        std::strncpy(pciBusId, "0000:00:00.0", static_cast<size_t>(len) - 1);
        pciBusId[len - 1] = '\0';
    }
    return hipSuccess;
}

inline hipError_t
hipHostMalloc(void **ptr, size_t size, unsigned int)
{
    if (!ptr) {
        return hipErrorMemoryAllocation;
    }
    *ptr = std::malloc(size);
    return *ptr ? hipSuccess : hipErrorMemoryAllocation;
}

inline hipError_t
hipHostFree(void *ptr)
{
    std::free(ptr);
    return hipSuccess;
}

inline hipError_t
hipDeviceSynchronize()
{
    return hipSuccess;
}

inline hipError_t
hipMalloc(void **ptr, size_t size)
{
    if (!ptr) {
        return hipErrorMemoryAllocation;
    }
    *ptr = std::malloc(size);
    return *ptr ? hipSuccess : hipErrorMemoryAllocation;
}

inline hipError_t
hipFree(void *ptr)
{
    std::free(ptr);
    return hipSuccess;
}

inline hipError_t
hipMemcpy(void *dst, const void *src, size_t size, hipMemcpyKind)
{
    std::memcpy(dst, src, size);
    return hipSuccess;
}

/* No GPU, so no real asynchrony: the copy is already complete by the time
 * this returns, and the paired hipEvent* calls below just report that. */
inline hipError_t
hipMemcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind, hipStream_t)
{
    std::memcpy(dst, src, size);
    return hipSuccess;
}

inline hipError_t
hipEventCreate(hipEvent_t *)
{
    return hipSuccess;
}

inline hipError_t
hipEventRecord(hipEvent_t, hipStream_t)
{
    return hipSuccess;
}

inline hipError_t
hipEventQuery(hipEvent_t)
{
    return hipSuccess;
}

inline hipError_t
hipEventDestroy(hipEvent_t)
{
    return hipSuccess;
}

/* Nothing on this build is ever real GPU memory. */
inline hipError_t
hipPointerGetAttributes(hipPointerAttribute_t *attr, const void *)
{
    if (attr) {
        attr->type = hipMemoryTypeHost;
    }
    return hipSuccess;
}

/* Never actually called on this build: isValidDeviceRange() (buffer.cpp)
 * only calls hipMemGetAddressRange() when hipPointerGetAttributes() first
 * reports device memory, which never happens here. Present only so the
 * HipOps table (hip-seam.h) has something to assign. */
inline hipError_t
hipMemGetAddressRange(hipDeviceptr_t *, size_t *, hipDeviceptr_t)
{
    return hipErrorNoDevice;
}
