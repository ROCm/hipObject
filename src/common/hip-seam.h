/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* HIP runtime seam.
 *
 * The library calls the HIP runtime, and the two HSA calls that export a
 * GPU buffer as a dma-buf, through HipRuntime, so unit tests can
 * substitute a fake (no GPU required). SystemHip, the production
 * implementation, forwards each call to the real entry point. The seam is
 * intentionally minimal: only the functions the library itself invokes
 * are listed.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <hip/hip_runtime_api.h>

namespace hipObj {

/* The methods are const, so one runtime can be shared by everything that
 * uses it. A fake that records its calls keeps what it records outside
 * the object, behind a pointer. */
class HipRuntime {
public:
    HipRuntime()          = default;
    virtual ~HipRuntime() = default;

    /* Copying through a reference to the base would slice */
    HipRuntime(const HipRuntime &)            = delete;
    HipRuntime &operator=(const HipRuntime &) = delete;
    HipRuntime(HipRuntime &&)                 = delete;
    HipRuntime &operator=(HipRuntime &&)      = delete;

    [[nodiscard]] virtual hipError_t getDevice(int *device) const                                  = 0;
    [[nodiscard]] virtual hipError_t getDeviceCount(int *count) const                              = 0;
    [[nodiscard]] virtual hipError_t deviceGetPCIBusId(char *pciBusId, int len, int device) const  = 0;
    [[nodiscard]] virtual hipError_t hostMalloc(void **ptr, size_t size, unsigned int flags) const = 0;
    [[nodiscard]] virtual hipError_t hostFree(void *ptr) const                                     = 0;
    [[nodiscard]] virtual hipError_t deviceSynchronize() const                                     = 0;
    [[nodiscard]] virtual hipError_t memcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind,
                                                 hipStream_t stream) const                         = 0;
    [[nodiscard]] virtual hipError_t eventCreate(hipEvent_t *event) const                          = 0;
    [[nodiscard]] virtual hipError_t eventRecord(hipEvent_t event, hipStream_t stream) const       = 0;
    [[nodiscard]] virtual hipError_t eventQuery(hipEvent_t event) const                            = 0;
    [[nodiscard]] virtual hipError_t eventDestroy(hipEvent_t event) const                          = 0;
    [[nodiscard]] virtual hipError_t pointerGetAttributes(hipPointerAttribute_t *attributes,
                                                          const void            *ptr) const                   = 0;
    [[nodiscard]] virtual hipError_t memGetAddressRange(hipDeviceptr_t *base, size_t *size,
                                                        hipDeviceptr_t ptr) const                  = 0;

    /* HSA, not HIP: hsa_amd_portable_export_dmabuf() and
     * hsa_amd_portable_close_dmabuf(). They return 0 or the failing
     * hsa_status_t as an int, so this header doesn't need the HSA
     * headers. */
    [[nodiscard]] virtual int exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const = 0;
    [[nodiscard]] virtual int closeDmabuf(int fd) const                                                   = 0;
};

/* The real HIP runtime */
class SystemHip final : public HipRuntime {
public:
    [[nodiscard]] hipError_t getDevice(int *device) const override;
    [[nodiscard]] hipError_t getDeviceCount(int *count) const override;
    [[nodiscard]] hipError_t deviceGetPCIBusId(char *pciBusId, int len, int device) const override;
    [[nodiscard]] hipError_t hostMalloc(void **ptr, size_t size, unsigned int flags) const override;
    [[nodiscard]] hipError_t hostFree(void *ptr) const override;
    [[nodiscard]] hipError_t deviceSynchronize() const override;
    [[nodiscard]] hipError_t memcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind,
                                         hipStream_t stream) const override;
    [[nodiscard]] hipError_t eventCreate(hipEvent_t *event) const override;
    [[nodiscard]] hipError_t eventRecord(hipEvent_t event, hipStream_t stream) const override;
    [[nodiscard]] hipError_t eventQuery(hipEvent_t event) const override;
    [[nodiscard]] hipError_t eventDestroy(hipEvent_t event) const override;
    [[nodiscard]] hipError_t pointerGetAttributes(hipPointerAttribute_t *attributes,
                                                  const void            *ptr) const override;
    [[nodiscard]] hipError_t memGetAddressRange(hipDeviceptr_t *base, size_t *size,
                                                hipDeviceptr_t ptr) const override;
    [[nodiscard]] int exportDmabuf(const void *ptr, size_t size, int *fd, uint64_t *offset) const override;
    [[nodiscard]] int closeDmabuf(int fd) const override;
};

/* Destroys a HIP event through the runtime that created it, which has to
 * outlive the pointer. Create the pointer with own() (ibv-ptr.h). */
class HipEventDeleter {
public:
    explicit HipEventDeleter(const HipRuntime &deleterRuntime) noexcept : runtime_(&deleterRuntime)
    {
    }
    void operator()(hipEvent_t event) const noexcept
    {
        (void)runtime_->eventDestroy(event);
    }

private:
    const HipRuntime *runtime_;
};

/* hipEvent_t is itself a pointer, to an opaque struct */
using HipEventPtr = std::unique_ptr<std::remove_pointer_t<hipEvent_t>, HipEventDeleter>;

} // namespace hipObj
