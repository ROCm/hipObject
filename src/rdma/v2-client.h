/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Internal v2 client entry points (called from src/hipobj.cpp under
 * v2::apiLock()). */

#pragma once

#include <cstdint>
#include <string>

#include "buffer.h"
#include "hipobj.h"

namespace hipObj {

/* The library-wide buffer map, defined in src/hipobj.cpp. */
extern BufferMap g_bufferMap;

namespace v2 {

/* Diagnostic marker carried in hipObjError_t::hipError when a
 * transfer fails Busy because its deadline expired. ABI-stable value;
 * the bridge and callers use it to distinguish expiry from
 * server-side backpressure. */
constexpr int kDiagDeadlineExpired = 0x54494D45; /* "TIME" */

/* hipObjInitV2 body. Returns a hipObjOpError_t value. */
int v2Init(hipObjConfigV2_t* config);

/* hipObjShutdown path for the v2 half. Returns a hipObjOpError_t
 * value; hipObjRdmaError when leftover poison stopped the teardown. */
int v2Shutdown();

/* Protection domain of the shared v2 device, or nullptr when the v2
 * half is not initialized. Buffer registration on a v2-only process
 * runs against this PD. */
struct ibv_pd* v2ProtectionDomain();

/* Shared body of hipObjGetV2/hipObjPutV2 (apiLock held by the
 * caller). Returns a hipObjOpError_t value. */
bool v2IsInitialized();

/* Entry timestamp for the public entry points, read from the same
 * injectable clock the transfer budget uses. Call before acquiring
 * the api lock so the wait counts against the budget. */
uint64_t v2EntryNowMs();

/* Name of the RDMA device the v2 stack selected at init, or an empty
 * string before init. Callers use this instead of minting a v1 RDMA
 * token to discover the NIC. */
const char* v2NicName();

/* Port and GID index the v2 data plane uses; meaningful after init
 * (port > 0, gidIndex >= 0). Lets the control plane bind the exact
 * interface backing the data-plane address handle. */
int v2SelectedPort();
int v2SelectedGidIndex();

/* Coherent interface selection snapshot: NIC name, port, and GID
 * index captured together, plus the init generation they belong to.
 * A transfer may only run when its generation still matches. */
struct InterfaceSnapshot {
  std::string nic;
  int port = 0;
  int gidIndex = -1;
  uint64_t generation = 0;
};
InterfaceSnapshot v2InterfaceSnapshot();

/* entryMs/haveEntryMs: when haveEntryMs is true the public entry
 * point captured the timestamp before waiting on the api lock, so
 * the lock wait counts against the whole-transfer budget; when
 * false (direct/internal callers, deterministic tests) v2Transfer
 * stamps its own entry with the injectable clock. */
int v2Transfer(int isPut, const char* bucket, const char* key, void* devPtr,
               uint64_t size, uint64_t offset, const char* query,
               hipObjOpsV2_t* ops, void* ctx, uint64_t entryMs,
               bool haveEntryMs, uint64_t snapshotGeneration = 0,
               bool haveSnapshot = false, int* diagOut = nullptr);

} // namespace v2
} // namespace hipObj
