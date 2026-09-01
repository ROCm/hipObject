/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <hip/hip_runtime.h>

#include "buffer.h"
#include "control.h"
#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-wrapper.h"
#include "rdma-topology.h"
#include "state.h"
#include "token.h"
#include "transport.h"
#include "v2-registry.h"
#include "v2-transport.h"

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

static bool buildRdmaToken(const void* devPtr, size_t size, off_t offset,
                           RdmaToken& token) {
  struct ibv_mr* mr = g_bufferMap.lookupMr(const_cast<void*>(devPtr));
  if (!mr || !g_conn.qp) {
    return false;
  }
  size_t regSize = g_bufferMap.lookupSize(const_cast<void*>(devPtr));
  if (offset < 0 || static_cast<size_t>(offset) + size > regSize) {
    return false;
  }
  token.transport = TRANSPORT_RC;
  token.qpNum = g_conn.qp->qp_num;
  std::memcpy(token.gid, g_conn.localGid.raw, 16);
  token.rkey = mr->rkey;
  token.remoteAddr = reinterpret_cast<uint64_t>(mr->addr) +
                     static_cast<uint64_t>(offset);
  token.length = static_cast<uint64_t>(size);
  token.portNum = g_conn.portNum;
  token.lid = 0;
  return true;
}

static int finishTransferAfterReply(const char* reply, size_t replyLen) {
  RdmaToken peerToken;
  int httpCode = 0;
  if (parsePeerTokenFromReply(reply, replyLen, peerToken, httpCode)) {
    if (connectRcPeer(g_conn, peerToken) != 0) {
      return -1;
    }
  }
  // A poll timeout or error completion is a failure: the transfer outcome
  // is unknown. With the current one-sided protocol the responder side
  // may not observe a completion at all (see issue #22), so this check
  // reports "no evidence of failure" rather than "transfer verified".
  if (pollCompletion(g_conn, -1, 5000) != 0) {
    return -1;
  }
  hipError_t err = hipObj::hipOps().hipDeviceSynchronize();
  return (err == hipSuccess) ? 0 : -1;
}

static hipObjError_t runRdmaTransfer(const void* devPtr, size_t size,
                                     off_t offset, hipObjOps_t* ops,
                                     void* ctx) {
  RdmaToken token;
  if (!buildRdmaToken(devPtr, size, offset, token)) {
    return {hipObjRdmaError, 0};
  }
  std::string encoded = encodeRdmaToken(token);
  if (injectRdmaToken(ops, ctx, encoded) != 0) {
    return {hipObjS3Error, 0};
  }
  char replyBuf[512];
  size_t replyLen = sizeof(replyBuf);
  int rdmaStatus = 0;
  if (receiveRdmaReplyRaw(ops, ctx, replyBuf, &replyLen, rdmaStatus) != 0 ||
      rdmaStatus != 0) {
    return {hipObjS3Error, 0};
  }
  if (finishTransferAfterReply(replyBuf, replyLen) != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
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
      case hipObjNotSupported:
        return "hipobj-rc-v2 not supported by server";
      case hipObjBusy:
        return "Server busy (backpressure)";
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
    hipError_t err = hipObj::hipOps().hipGetDevice(&gpuDevice);
    if (err != hipSuccess) {
      return {hipObjRdmaError, static_cast<int>(err)};
    }
  }
  const char* devName = nullptr;
  int nicIndex = hipObj::GetClosestNicToGpu(gpuDevice,
                                            config->nicHint ? config->nicHint
                                                            : nullptr,
                                            &devName);
  if (nicIndex < 0) {
    // GPU topology lookup failed (no GPU or no matching NIC). When a NIC name
    // hint is provided, try opening it directly without GPU topology so the
    // library works in GPU-less environments (e.g. CI with emulated RDMA).
    if (config->nicHint && config->nicHint[0] != '\0') {
      devName = config->nicHint;
      nicIndex = 0;
    } else {
      return {hipObjNicNotFound, 0};
    }
  }
  int ret = (devName) ? hipObj::openRdmaDeviceByName(devName, hipObj::g_conn)
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
  std::lock_guard<std::mutex> apiGuard(hipObj::v2::apiLock());
  /* v2 first: release every connection (destroy retries included);
   * leftover poison must stop the teardown so the failure is
   * visible instead of violating the PD/context lifetime rule. */
  bool poisonLeft = false;
  hipObj::v2::ConnectionRegistry& reg = hipObj::v2::registry();
  std::vector<hipObj::v2::ConnId> ids;
  reg.forEachId([&ids](hipObj::v2::ConnId id) {
    ids.push_back(id);
  });
  for (auto id : ids) {
    int rc = hipObj::v2::releaseConnection(id);
    if (rc == hipObj::v2::kReleaseLeftover) {
      poisonLeft = true;
    }
  }
  if (poisonLeft || reg.size() > 0) {
    return {hipObjRdmaError, 0};
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
  int ret = hipObj::g_bufferMap.registerBuffer(devPtr, size, hipObj::g_conn.pd);
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

hipObjError_t hipObjGet(hipObjHandle_t handle, void* devPtr, size_t size,
                        off_t offset, hipObjOps_t* ops, void* ctx) try {
  (void)handle;
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!ops) {
    return {hipObjInvalidValue, 0};
  }
  if (!hipObj::g_bufferMap.lookupMr(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  return hipObj::runRdmaTransfer(devPtr, size, offset, ops, ctx);
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjPut(hipObjHandle_t handle, const void* devPtr, size_t size,
                        off_t offset, hipObjOps_t* ops, void* ctx) try {
  (void)handle;
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!ops) {
    return {hipObjInvalidValue, 0};
  }
  if (!hipObj::g_bufferMap.lookupMr(const_cast<void*>(devPtr))) {
    return {hipObjBufNotRegistered, 0};
  }
  return hipObj::runRdmaTransfer(devPtr, size, offset, ops, ctx);
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjGetRdmaToken(const void* devPtr, size_t size, int op,
                                 char** outToken) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!devPtr || !outToken || size == 0) {
    return {hipObjInvalidValue, 0};
  }
  if (op != HIPOBJ_RDMA_OP_PUT && op != HIPOBJ_RDMA_OP_GET) {
    return {hipObjInvalidValue, 0};
  }
  if (!hipObj::g_bufferMap.lookupMr(const_cast<void*>(devPtr))) {
    return {hipObjBufNotRegistered, 0};
  }
  hipObj::RdmaToken token;
  if (!hipObj::buildRdmaToken(devPtr, size, 0, token)) {
    return {hipObjRdmaError, 0};
  }
  std::string encoded = hipObj::encodeRdmaToken(token);
  char* copy = static_cast<char*>(std::malloc(encoded.size() + 1));
  if (!copy) {
    return {hipObjInternalError, 0};
  }
  std::memcpy(copy, encoded.c_str(), encoded.size() + 1);
  *outToken = copy;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjPutRdmaToken(char* token) try {
  if (!token) {
    return {hipObjInvalidValue, 0};
  }
  std::free(token);
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjParseRdmaReply(const char* reply, size_t replyLen,
                                   int* httpCode) try {
  if (!reply || !httpCode) {
    return {hipObjInvalidValue, 0};
  }
  int code = 0;
  if (!hipObj::parseRdmaReplyHttpCode(reply, replyLen, code)) {
    return {hipObjInvalidValue, 0};
  }
  *httpCode = code;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjTokenClientNic(const char* token, char* nicIp,
                                   size_t nicIpLen) try {
  if (!token || !nicIp || nicIpLen == 0) {
    return {hipObjInvalidValue, 0};
  }
  if (!hipObj::parseClientNicFromTokenHex(token, nicIp, nicIpLen)) {
    return {hipObjInvalidValue, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

const char* hipObjGetVersionString(void) try {
  static char buf[32];
  snprintf(buf, sizeof(buf), "%d.%d.%d", HIPOBJ_VERSION_MAJOR,
           HIPOBJ_VERSION_MINOR, HIPOBJ_VERSION_PATCH);
  return buf;
} catch (...) {
  return "0.0.0";
}

} // extern "C"
