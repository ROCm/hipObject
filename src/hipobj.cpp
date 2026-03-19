/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"
#include "buffer.h"
#include "control.hpp"
#include "hipobj-private.h"
#include "ibv-wrapper.hpp"
#include "rdma-topology.hpp"
#include "state.h"
#include "token.hpp"
#include "transport.hpp"

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstring>

namespace hipObj {

static BufferMap g_bufferMap;
static RcConnection g_conn;

static hipObjError_t handleException() {
  try {
    throw;
  } catch (const std::exception&) {
    return {hipObjInternalError, 0};
  } catch (...) {
    return {hipObjInternalError, 0};
  }
}

} // namespace hipObj

extern "C" {

const char* hipObjGetErrorString(hipObjOpError_t err) {
  try {
    switch (err) {
      case hipObjSuccess:
        return "Success";
      case hipObjInvalidValue:
        return "Invalid value";
      case hipObjNotInitialized:
        return "Not initialized";
      case hipObjAlreadyInitialized:
        return "Already initialized";
      case hipObjRdmaError:
        return "RDMA error";
      case hipObjS3Error:
        return "S3 error";
      case hipObjBufNotRegistered:
        return "Buffer not registered";
      case hipObjBufAlreadyRegistered:
        return "Buffer already registered";
      case hipObjNicNotFound:
        return "NIC not found";
      case hipObjDmabufNotSupported:
        return "dmabuf not supported";
      case hipObjSizeTooLarge:
        return "Size too large";
      case hipObjInternalError:
        return "Internal error";
      default:
        return "Unknown error";
    }
  } catch (...) {
    return "Unknown error";
  }
}

hipObjError_t hipObjInit(hipObjConfig_t* config) try {
  if (!config) {
    return {hipObjInvalidValue, 0};
  }
  hipObj::DriverState& state = hipObj::getState();
  if (state.initialized) {
    return {hipObjAlreadyInitialized, 0};
  }
  if (!hipObj::ibv.is_initialized) {
    return {hipObjRdmaError, 0};
  }
  int gpuDevice = config->gpuDevice;
  if (gpuDevice < 0) {
    hipError_t err = hipGetDevice(&gpuDevice);
    if (err != hipSuccess) {
      return {hipObjRdmaError, static_cast<int>(err)};
    }
  }
  const char* devName = nullptr;
  int nicIndex = hipObj::GetClosestNicToGpu(
    gpuDevice,
    config->nicHint ? config->nicHint : nullptr,
    &devName);
  if (nicIndex < 0) {
    return {hipObjNicNotFound, 0};
  }
  int ret = (devName)
    ? hipObj::openRdmaDeviceByName(devName, hipObj::g_conn)
    : hipObj::openRdmaDevice(nicIndex, hipObj::g_conn);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  ret = hipObj::createRcQp(hipObj::g_conn, 256, 128, 128);
  if (ret != 0) {
    hipObj::closeRdmaDevice(hipObj::g_conn);
    return {hipObjRdmaError, 0};
  }
  ret = hipObj::transitionQpToInit(hipObj::g_conn);
  if (ret != 0) {
    hipObj::closeRdmaDevice(hipObj::g_conn);
    return {hipObjRdmaError, 0};
  }
  state.initialized = true;
  state.gpuDevice = gpuDevice;
  state.endpoint = config->endpoint ? config->endpoint : "";
  state.region = config->region ? config->region : "";
  state.nicHint = config->nicHint ? config->nicHint : "";
  state.nicIndex = nicIndex;
  state.flags = config->flags;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjShutdown(void) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return HIPOBJ_SUCCESS;
  }
  hipObj::g_bufferMap.deregisterAll();
  hipObj::closeRdmaDevice(hipObj::g_conn);
  state.initialized = false;
  state.gpuDevice = 0;
  state.endpoint.clear();
  state.region.clear();
  state.nicHint.clear();
  state.nicIndex = -1;
  state.flags = 0;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjBufRegister(void* devPtr, size_t size) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (size > hipObj::MAX_MR_SIZE) {
    return {hipObjSizeTooLarge, 0};
  }
  if (hipObj::g_bufferMap.isRegistered(devPtr)) {
    return {hipObjBufAlreadyRegistered, 0};
  }
  int ret = hipObj::g_bufferMap.registerBuffer(
    devPtr, size, hipObj::g_conn.pd);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjBufDeregister(void* devPtr) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!hipObj::g_bufferMap.isRegistered(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  int ret = hipObj::g_bufferMap.deregisterBuffer(devPtr);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjGet(hipObjHandle_t handle, void* devPtr,
                        size_t size, off_t offset,
                        hipObjOps_t* ops, void* ctx) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!ops) {
    return {hipObjInvalidValue, 0};
  }
  struct ibv_mr* mr = hipObj::g_bufferMap.lookupMr(devPtr);
  if (!mr) {
    return {hipObjBufNotRegistered, 0};
  }
  hipObj::RdmaToken token;
  token.transport = hipObj::TRANSPORT_RC;
  token.qpNum = hipObj::g_conn.qp->qp_num;
  std::memcpy(token.gid, hipObj::g_conn.localGid.raw, 16);
  token.rkey = mr->rkey;
  token.remoteAddr = reinterpret_cast<uint64_t>(mr->addr);
  token.length = static_cast<uint64_t>(size);
  token.portNum = hipObj::g_conn.portNum;
  token.lid = 0;
  std::string encoded = hipObj::encodeRdmaToken(token);
  int ret = hipObj::injectRdmaToken(ops, ctx, encoded);
  if (ret != 0) {
    return {hipObjS3Error, 0};
  }
  int rdmaStatus = 0;
  ret = hipObj::receiveRdmaReply(ops, ctx, rdmaStatus);
  if (ret != 0 || rdmaStatus != 0) {
    return {hipObjS3Error, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjPut(hipObjHandle_t handle, const void* devPtr,
                       size_t size, off_t offset,
                       hipObjOps_t* ops, void* ctx) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!ops) {
    return {hipObjInvalidValue, 0};
  }
  struct ibv_mr* mr =
    hipObj::g_bufferMap.lookupMr(const_cast<void*>(devPtr));
  if (!mr) {
    return {hipObjBufNotRegistered, 0};
  }
  hipObj::RdmaToken token;
  token.transport = hipObj::TRANSPORT_RC;
  token.qpNum = hipObj::g_conn.qp->qp_num;
  std::memcpy(token.gid, hipObj::g_conn.localGid.raw, 16);
  token.rkey = mr->rkey;
  token.remoteAddr = reinterpret_cast<uint64_t>(mr->addr);
  token.length = static_cast<uint64_t>(size);
  token.portNum = hipObj::g_conn.portNum;
  token.lid = 0;
  std::string encoded = hipObj::encodeRdmaToken(token);
  int ret = hipObj::injectRdmaToken(ops, ctx, encoded);
  if (ret != 0) {
    return {hipObjS3Error, 0};
  }
  int rdmaStatus = 0;
  ret = hipObj::receiveRdmaReply(ops, ctx, rdmaStatus);
  if (ret != 0 || rdmaStatus != 0) {
    return {hipObjS3Error, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

const char* hipObjGetVersionString(void) try {
  static char buf[32];
  snprintf(buf, sizeof(buf), "%d.%d.%d",
           HIPOBJ_VERSION_MAJOR,
           HIPOBJ_VERSION_MINOR,
           HIPOBJ_VERSION_PATCH);
  return buf;
} catch (...) {
  return "0.0.0";
}

} // extern "C"
