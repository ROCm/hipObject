/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Pins hipobj.h's API as it is in hipObject 0.1.0. Nothing here may change
 * once a version is released: a change to a function or struct gets a
 * new, versioned name instead (see the "API Versions" group in hipobj.h).
 * Each check is a static_assert, so a change that breaks one fails the
 * build.
 *
 * Add the functions, structs, error codes, and macros that a new version
 * adds, but don't change what's here. Until 0.1.0 is released, an
 * approved API change can still change these to match. */

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "hipobj.h"

namespace {

// ---- Structs -----------------------------------------------------------

/* Copies of the structs as they are in 0.1.0. Each field of the public
 * struct has the copy's offset and type, and the struct has its size. */

struct ErrorV0_1_0 {
    hipObjOpError_t opError;
    int             hipError;
};

static_assert(sizeof(hipObjError_t) == sizeof(ErrorV0_1_0));
static_assert(offsetof(hipObjError_t, opError) == offsetof(ErrorV0_1_0, opError));
static_assert(offsetof(hipObjError_t, hipError) == offsetof(ErrorV0_1_0, hipError));
static_assert(std::is_same_v<decltype(hipObjError_t::opError), decltype(ErrorV0_1_0::opError)>);
static_assert(std::is_same_v<decltype(hipObjError_t::hipError), decltype(ErrorV0_1_0::hipError)>);

struct OpsV0_1_0 {
    int (*sendRequest)(void *ctx, const char *token, size_t tokenLen);
    int (*recvReply)(void *ctx, char *reply, size_t *replyLen);
};

static_assert(sizeof(hipObjOps_t) == sizeof(OpsV0_1_0));
static_assert(offsetof(hipObjOps_t, sendRequest) == offsetof(OpsV0_1_0, sendRequest));
static_assert(offsetof(hipObjOps_t, recvReply) == offsetof(OpsV0_1_0, recvReply));
static_assert(std::is_same_v<decltype(hipObjOps_t::sendRequest), decltype(OpsV0_1_0::sendRequest)>);
static_assert(std::is_same_v<decltype(hipObjOps_t::recvReply), decltype(OpsV0_1_0::recvReply)>);

struct ConfigV0_1_0 {
    uint32_t    flags;
    int         gpuDevice;
    const char *nicHint;
};

static_assert(sizeof(hipObjConfig_t) == sizeof(ConfigV0_1_0));
static_assert(offsetof(hipObjConfig_t, flags) == offsetof(ConfigV0_1_0, flags));
static_assert(offsetof(hipObjConfig_t, gpuDevice) == offsetof(ConfigV0_1_0, gpuDevice));
static_assert(offsetof(hipObjConfig_t, nicHint) == offsetof(ConfigV0_1_0, nicHint));
static_assert(std::is_same_v<decltype(hipObjConfig_t::flags), decltype(ConfigV0_1_0::flags)>);
static_assert(std::is_same_v<decltype(hipObjConfig_t::gpuDevice), decltype(ConfigV0_1_0::gpuDevice)>);
static_assert(std::is_same_v<decltype(hipObjConfig_t::nicHint), decltype(ConfigV0_1_0::nicHint)>);

static_assert(std::is_same_v<hipObjHandle_t, void *>);

// ---- Functions ---------------------------------------------------------

/* Each function's type, which is its return type and parameter types */

static_assert(std::is_same_v<decltype(&hipObjGetErrorString), const char *(*)(hipObjOpError_t)>);
static_assert(std::is_same_v<decltype(&hipObjInit), hipObjError_t (*)(hipObjConfig_t *)>);
static_assert(std::is_same_v<decltype(&hipObjShutdown), hipObjError_t (*)()>);
static_assert(std::is_same_v<decltype(&hipObjBufRegister), hipObjError_t (*)(void *, size_t)>);
static_assert(std::is_same_v<decltype(&hipObjBufRegisterHost), hipObjError_t (*)(void *, size_t)>);
static_assert(std::is_same_v<decltype(&hipObjBufDeregister), hipObjError_t (*)(void *)>);
static_assert(std::is_same_v<decltype(&hipObjGet), hipObjError_t (*)(hipObjHandle_t, void *, size_t, size_t,
                                                                     hipObjOps_t *, void *)>);
static_assert(std::is_same_v<decltype(&hipObjPut), hipObjError_t (*)(hipObjHandle_t, const void *, size_t,
                                                                     size_t, hipObjOps_t *, void *)>);
static_assert(std::is_same_v<decltype(&hipObjBufSync), hipObjError_t (*)(void *, size_t, size_t, int)>);
static_assert(
    std::is_same_v<decltype(&hipObjGetRdmaToken), hipObjError_t (*)(const void *, size_t, int, char **)>);
static_assert(std::is_same_v<decltype(&hipObjPutRdmaToken), hipObjError_t (*)(char *)>);
static_assert(
    std::is_same_v<decltype(&hipObjParseRdmaReply), hipObjError_t (*)(const char *, size_t, int *)>);
static_assert(
    std::is_same_v<decltype(&hipObjTokenClientNic), hipObjError_t (*)(const char *, char *, size_t)>);
static_assert(std::is_same_v<decltype(&hipObjGetVersionString), const char *(*)()>);
static_assert(
    std::is_same_v<decltype(&hipObjGetVersion), hipObjError_t (*)(unsigned *, unsigned *, unsigned *)>);

// ---- Values ------------------------------------------------------------

static_assert(HIPOBJ_BASE_ERR == 6000);
static_assert(hipObjSuccess == 0);
static_assert(hipObjInvalidValue == 6001);
static_assert(hipObjNotInitialized == 6002);
static_assert(hipObjAlreadyInitialized == 6003);
static_assert(hipObjRdmaError == 6004);
static_assert(hipObjS3Error == 6005);
static_assert(hipObjBufNotRegistered == 6006);
static_assert(hipObjBufAlreadyRegistered == 6007);
static_assert(hipObjNicNotFound == 6008);
static_assert(hipObjDmabufNotSupported == 6009);
static_assert(hipObjSizeTooLarge == 6010);
static_assert(hipObjInternalError == 6011);
static_assert(hipObjBusy == 6012);

static_assert(HIPOBJ_RDMA_OP_PUT == 0);
static_assert(HIPOBJ_RDMA_OP_GET == 1);
static_assert(HIPOBJ_RDMA_REPLY_NOT_IMPLEMENTED == 501);
static_assert(HIPOBJ_SYNC_TO_HOST == 0);
static_assert(HIPOBJ_SYNC_TO_DEVICE == 1);

} // namespace
