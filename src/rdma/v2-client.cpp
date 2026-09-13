/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* hipobj-rc-v2 client session driver: the implementation behind
 * hipObjInitV2/hipObjGetV2/hipObjPutV2.
 *
 * The driver owns the RDMA session; the consumer owns the control
 * plane (HTTP/SigV4) through the hipObjOpsV2_t callbacks. The READY
 * exchange is split (sendReadyRequest writes the request, the data
 * phase runs while the exchange is pending, finishReady reads the
 * response), so the data motion keeps the overlap the reference
 * client uses while the library stays free of transport code.
 *
 * PUT uses the staging-push model every pinned server implements:
 * the PREPARE reply advertises the server staging MR and the client
 * posts RDMA WRITE WITH IMMEDIATE to it. The pull model (server
 * READs from the client buffer) is proposed but not implemented by
 * any peer today.
 *
 * Everything here runs under v2::apiLock() with no worker threads:
 * helpers join before the entry point returns.
 */

#include "v2-client.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <thread>

#include "ibv-wrapper.h"
#include "state.h"
#include "rdma-topology.h"
#include "token.h"
#include "transport.h"
#include "v2-clock.h"
#include "v2-random.h"
#include "v2-registry.h"
#include "v2-state.h"
#include "v2-transport.h"
#include "v2-wire.h"

namespace hipObj {
namespace v2 {

namespace {

constexpr uint32_t kDefaultTransferDeadlineMs = 60'000;
constexpr uint32_t kDefaultConnectDeadlineMs = 10'000;
constexpr uint32_t kDefaultCancelBudgetMs = 1'000;
/* Diagnostic carried in hipObjError_t.hipError when the operation
 * failed because the whole-transfer deadline expired (the C ABI has
 * no dedicated timeout code; Busy + this marker preserves the fact
 * without changing the enum). */

constexpr uint32_t kPutGraceMs = 100;

uint64_t steadyNowMs() {
  /* Route every v2 time read through the injectable clock so unit
   * tests control expiry deterministically. */
  return clockSource().nowMs();
}

uint64_t remaining(uint64_t deadlineMs) {
  const uint64_t now = steadyNowMs();
  return now >= deadlineMs ? 0 : deadlineMs - now;
}

/* ---- per-process v2 state (guarded by apiLock) ---- */

struct V2State {
  bool initialized = false;
  std::string controlEndpoint;
  uint32_t connectDeadlineMs = 0;
  uint32_t transferDeadlineMs = 0;
  uint32_t cancelBudgetMs = 0;
  /* RDMA device name selected at init; v2 callers query it instead of
   * minting a v1 RDMA token just to learn the NIC. */
  std::string nicName;
  /* Port and GID index the data plane actually uses (recorded at init
   * so the control-plane interface binding can resolve the matching
   * netdev instead of an arbitrary one). */
  int selectedPort = 0;
  int selectedGidIndex = -1;
  /* Shared device handle: created by hipObjInitV2, referenced by every
   * connection. Shutdown reclaims it. */
  DeviceHandle* device = nullptr;
  /* Bumped by every init and every shutdown. Interface snapshots taken
   * outside the transfer lock carry the generation they were read
   * under; transfer admission rejects a stale snapshot instead of
   * mixing selections from different initializations. */
  uint64_t initGeneration = 0;
};

/* Emergency owner for verbs objects whose registry parking failed
 * (allocation failure or registry full). Fixed capacity, no further
 * allocation, guarded by apiLock: a survivor recorded here is visible
 * to shutdown instead of becoming ownerless. */
struct EmergencySlot {
  RcConnV2 conn;
  uint64_t reservationId = 0;
  bool used = false;
  /* Reserved before verbs creation so a later parking failure can
   * never find the owner full: admission refuses new transfers
   * instead of stranding survivors. */
  bool reserved = false;
};
constexpr size_t kEmergencySlots = 8;
EmergencySlot g_emergencySlots[kEmergencySlots];

/* Claim one emergency slot for the transfer about to create verbs
 * objects. Returns a handle (index + 1) or 0 when all are taken. */
static size_t reserveEmergencySlot() {
  for (size_t i = 0; i < kEmergencySlots; ++i) {
    if (!g_emergencySlots[i].used && !g_emergencySlots[i].reserved) {
      g_emergencySlots[i].reserved = true;
      return i + 1;
    }
  }
  return 0;
}

static void releaseEmergencyReservation(size_t handle) {
  if (handle == 0 || handle > kEmergencySlots) {
    return;
  }
  auto& slot = g_emergencySlots[handle - 1];
  if (slot.reserved && !slot.used) {
    slot.reserved = false;
  }
}

static bool parkEmergency(size_t handle, RcConnV2& conn,
                          uint64_t reservationId) {
  if (handle == 0 || handle > kEmergencySlots) {
    return false;
  }
  auto& slot = g_emergencySlots[handle - 1];
  if (!slot.reserved || slot.used) {
    return false;
  }
  slot.conn = conn;
  slot.reservationId = reservationId;
  slot.used = true;
  slot.reserved = false;
  return true;
}

static void drainEmergencySlots() {
  for (auto& slot : g_emergencySlots) {
    if (!slot.used) {
      continue;
    }
    bool qpOk = false;
    bool cqOk = false;
    destroyRcConnV2(slot.conn, &qpOk, &cqOk);
    if (qpOk && slot.reservationId != 0) {
      /* The reservation guards a (qpn, psn) pair; with the QP gone
       * the pair can never reach the wire, so settle it now instead
       * of waiting for the CQ recovery too. */
      registry().retired().unreserve(slot.reservationId);
      slot.reservationId = 0;
    }
    if (qpOk && cqOk) {
      /* Only a fully destroyed connection may release its recovery
       * owner: a surviving CQ still needs this slot so a later
       * shutdown can retry its destroy. */
      slot.used = false;
    }
    /* A survivor that still cannot be fully destroyed stays
     * recorded; the next shutdown retries. */
  }
}

V2State& v2State() {
  static V2State state;
  return state;
}

/* ---- deadline-bounded CQ poll ---- */

bool pollDeadline(struct ibv_cq* cq, int expectedOpcode, uint64_t deadlineMs,
                  struct ibv_wc& wc) {
  for (;;) {
    struct ibv_wc cur = {};
    const int n = ibv.poll_cq(cq, 1, &cur);
    if (n > 0) {
      wc = cur;
      return wc.status == IBV_WC_SUCCESS &&
             (expectedOpcode == -1 || wc.opcode == expectedOpcode);
    }
    if (n < 0) {
      return false;
    }
    if (steadyNowMs() >= deadlineMs) {
      return false;
    }
    usleep(1000);
  }
}

/* One bounded wait sliced so the deadline is always honored. */
void boundedSleep(uint64_t untilMs, uint64_t deadlineMs) {
  for (;;) {
    const uint64_t now = steadyNowMs();
    const uint64_t end = untilMs < deadlineMs ? untilMs : deadlineMs;
    if (now >= end || now >= deadlineMs) {
      return;
    }
    uint64_t slice = end - now;
    if (slice > 10) {
      slice = 10;
    }
    usleep(static_cast<useconds_t>(slice * 1000));
  }
}

/* ---- request construction (shared by all phases) ---- */

void fillCommonRequest(hipObjTransferReqV2_t& req, const char* method,
                       const char* bucket, const char* key, uint64_t size,
                       uint64_t offset, const char* query, uint32_t cookie,
                       uint32_t clientPsn, uint64_t deadlineMs,
                       uint64_t remainingMs) {
  std::memset(&req, 0, sizeof(req));
  req.method = method;
  req.bucket = bucket;
  req.key = key;
  req.query = query;
  req.size = size;
  req.offset = offset;
  req.cookie = cookie;
  req.clientPsn = clientPsn;
  /* deadlineMs is the remaining whole-transfer budget per the header
   * contract, not the absolute steady-clock deadline. */
  req.deadlineMs = static_cast<uint32_t>(
    remainingMs > 0xffffffff ? 0xffffffff : remainingMs);
  req.remainingMs = req.deadlineMs;
}

struct SessionResources {
  DeviceHandle* dh = nullptr;
  RcConnV2 conn;
  ConnId id = 0;
  bool inserted = false;
  void* pinnedBuffer = nullptr;
};

/* Creates the per-transfer QP/CQ pair as a registry entry with a
 * retired-ring reservation, and pins the buffer MR. Returns 0 on
 * success; on failure every partial resource is rolled back. */
/* Ownership rules for acquireSession failure paths: every reference
 * this call acquired (MR pin, registry slot, retirement reservation,
 * device reference) is released exactly once, HERE, before returning.
 * res is left describing nothing (no partial owner), so releaseSession
 * never re-releases. The only exception is a survived-but-undestroyable
 * verbs object (rollbackFailed), reported as -2 and still owned here:
 * those are recorded below as a poisoned registry entry so the reaper
 * path owns them. */
int acquireSession(void* devPtr, SessionResources& res,
                   size_t emergencyHandle) {
  ConnectionRegistry& reg = registry();
  /* Retire stale (qpn, psn) records first: a full ring would turn
   * every teardown Busy otherwise, and expired records are free to
   * reclaim (the reuse window has passed). */
  reg.retired().collectExpired(clockSource().nowMs());
  if (!g_bufferMap.acquireMrRef(devPtr)) {
    return -1;
  }
  if (!reg.reserveSlot()) {
    g_bufferMap.releaseMrRef(devPtr);
    return -1;
  }
  /* Reserve retirement capacity BEFORE creating the QP: the pair is
   * only exposed to the wire once the retirement slot exists, so a
   * later teardown never blocks on an exhausted ring. */
  const uint64_t retireRid = reg.retired().reserve();
  if (retireRid == 0) {
    g_bufferMap.releaseMrRef(devPtr);
    reg.unreserveSlot();
    return -1;
  }
  bool rollbackFailed = false;
  const int cret = createRcConnV2(v2State().device, res.conn,
                                  &rollbackFailed);
  if (cret != 0) {
    if (!rollbackFailed) {
      /* Clean rollback: nothing survived, release every reference
       * this call acquired. */
      reg.retired().unreserve(retireRid);
      reg.unreserveSlot();
      g_bufferMap.releaseMrRef(devPtr);
      return -1;
    }
    /* Verbs destroy failed during rollback: a CQ survived without a
     * QP, and createRcConnV2 never incremented the device reference
     * (that happens only after QP success). Park the survivor as a
     * poisoned entry that owns the slot, the reservation, and the MR
     * pin, but NOT a device reference; the slot and reservation stay
     * reserved here so the insert below succeeds. */
    ConnectionEntryV2 entry;
    entry.conn = res.conn;
    entry.device = v2State().device;
    entry.reservationId = retireRid;
    entry.pinnedBuffer = devPtr;
    entry.poisoned = true;
    entry.holdsDeviceRef = false;
    bool cqParkThrew = false;
    try {
      res.id = reg.insert(std::move(entry));
    } catch (...) {
      cqParkThrew = true;
    }
    if (cqParkThrew || res.id == 0) {
      /* Registry full or the allocation failed again: try to destroy
       * the surviving CQ directly; if that also fails, record it
       * with the emergency owner so shutdown (not luck) owns it. */
      bool qpIgnored = false;
      bool cqOk = false;
      destroyRcConnV2(res.conn, &qpIgnored, &cqOk);
      bool settled = cqOk || parkEmergency(emergencyHandle, res.conn, retireRid);
      g_bufferMap.releaseMrRef(devPtr);
      if (cqOk) {
        reg.retired().unreserve(retireRid);
      }
      reg.unreserveSlot();
      if (!settled) {
        /* Should be unreachable: admission reserves an emergency
         * slot before verbs creation, so parking always has a home.
         * Report the failure rather than dropping the survivor. */
        return -2;
      }
      return -2;
    }
    res.dh = nullptr; /* no device reference was acquired */
    /* The poisoned entry owns the pin now; res must not double-release. */
    res.pinnedBuffer = nullptr;
    res.inserted = true;
    return -2;
  }
  ConnectionEntryV2 entry;
  entry.conn = res.conn;
  entry.device = v2State().device;
  entry.clientPsn = 0;
  entry.reservationId = retireRid;
  entry.pinnedBuffer = devPtr;
  bool insertThrew = false;
  try {
    res.id = reg.insert(std::move(entry));
  } catch (...) {
    /* The node allocation failed with a live QP/CQ pair still owned
     * here: destroy it before releasing the references, and park the
     * pair when the destroy itself fails so shutdown has an owner to
     * reclaim. The moved-from entry's raw members are unspecified
     * after the throw; res.conn is the surviving handle. */
    insertThrew = true;
  }
  if (insertThrew || res.id == 0) {
    bool qpOk = false;
    bool cqOk = false;
    destroyRcConnV2(res.conn, &qpOk, &cqOk);
    /* The destroy failed: the QP keeps its retirement reservation
     * (recorded only on successful destroy, so keep the reservation
     * with a poisoned entry instead of unreserving). */
    if (!qpOk || !cqOk) {
      /* Exactly one side survived. The reservation stays with the
       * surviving QP when the QP destroy failed; when the QP was
       * destroyed the retirement reservation is settled below. The
       * device reference is released immediately, so a parked entry
       * never owns one. */
      ConnectionEntryV2 poisoned;
      poisoned.conn = res.conn;
      poisoned.device = v2State().device;
      poisoned.reservationId = qpOk ? 0 : retireRid;
      poisoned.pinnedBuffer = nullptr; /* MR ref released below */
      poisoned.poisoned = true;
      poisoned.holdsDeviceRef = false;
      if (qpOk) {
        reg.retired().unreserve(retireRid);
      }
      bool parkedOk = true;
      try {
        res.id = reg.insert(std::move(poisoned));
      } catch (...) {
        res.id = 0;
      }
      if (res.id == 0) {
        /* Allocation failed twice: give the survivor to the emergency
         * owner so shutdown still reclaims it. */
        parkedOk = parkEmergency(emergencyHandle, res.conn,
                                 qpOk ? 0 : retireRid);
      }
      releaseDevice(v2State().device);
      g_bufferMap.releaseMrRef(devPtr);
      reg.unreserveSlot();
      return (res.id != 0 || parkedOk) ? -2 : -1;
    }
    reg.retired().unreserve(retireRid);
    releaseDevice(v2State().device);
    g_bufferMap.releaseMrRef(devPtr);
    reg.unreserveSlot();
    return -1;
  }
  res.dh = v2State().device;
  res.pinnedBuffer = devPtr;
  res.inserted = true;
  if (transitionQpToInitV2(res.dh, res.conn) != 0) {
    /* The entry owns the QP and the pin; releaseSession will run
     * releaseConnection on it (id is set, inserted is true). */
    return -1;
  }
  return 0;
}

/* Releases the session entry. Returns the release outcome so callers
 * can map Busy/leftover per the documented policy. */
int releaseSession(SessionResources& res) {
  if (res.pinnedBuffer != nullptr) {
    /* The MR pin is held until the release fully succeeds; a
     * Busy/leftover outcome keeps the entry (and its pin) alive. */
    int rc = 0;
    if (!res.inserted) {
      /* Unreachable in practice: acquireSession clears pinnedBuffer
       * on every not-inserted failure path, so a non-inserted session
       * holds nothing here. Destroy any stragglers defensively; no
       * device reference was ever acquired for them. */
      rc = 0;
      bool qpOk = false;
      bool cqOk = false;
      destroyRcConnV2(res.conn, &qpOk, &cqOk);
      if (!qpOk || !cqOk) {
        rc = kReleaseLeftover;
      }
    } else {
      /* Take the entry's MR pin and device-ref ownership out before
       * the release so both can be settled here (this translation
       * unit owns the buffer map linkage) once the outcome is known. */
      void* entryPin = nullptr;
      bool holdsDeviceRef = true;
      registry().withEntry(res.id, [&](ConnectionEntryV2& e) {
        entryPin = e.pinnedBuffer;
        holdsDeviceRef = e.holdsDeviceRef;
        e.pinnedBuffer = nullptr;
        e.holdsDeviceRef = false;
      });
      rc = releaseConnection(res.id);
      /* The device reference taken by createRcConnV2 is returned when
       * the connection release consumed the entry. Busy and leftover
       * outcomes keep the entry (and its pin, device reference, and
       * ownership flags) alive for the retry path. */
      if (rc == kReleaseOk) {
        if (entryPin != nullptr) {
          g_bufferMap.releaseMrRef(entryPin);
        }
        if (holdsDeviceRef) {
          releaseDevice(res.dh != nullptr ? res.dh : v2State().device);
        }
      } else {
        registry().withEntry(res.id, [&](ConnectionEntryV2& e) {
          e.pinnedBuffer = entryPin;
          e.holdsDeviceRef = holdsDeviceRef;
        });
      }
    }
    if (rc == kReleaseOk) {
      res.pinnedBuffer = nullptr;
    }
    return rc;
  }
  return kReleaseOk;
}
} // namespace

/* Maps an internal failure code to the public error with the
 * non-quiesced-release policy applied. releaseRc is the outcome of
 * releaseSession(); when it failed, the entry stays owned by the
 * registry and the buffer stays pinned, so the transfer surfaces
 * hipObjInternalError regardless of the wire outcome. */
hipObjError_t finalizeOutcome(hipObjError_t wireOutcome, int releaseRc) {
  if (releaseRc == kReleaseOk) {
    return wireOutcome;
  }
  /* Non-quiesced release overrides the status (safety first) but the
   * deadline fact must survive: the caller can still distinguish an
   * expired transfer from other internal failures. */
  return {hipObjInternalError,
          wireOutcome.hipError == kDiagDeadlineExpired ? kDiagDeadlineExpired
                                                       : 0};
}

bool v2IsInitialized() { return v2State().initialized; }

uint64_t v2EntryNowMs() { return steadyNowMs(); }

const char* v2NicName() { return v2State().nicName.c_str(); }

int v2SelectedPort() { return v2State().selectedPort; }

int v2SelectedGidIndex() { return v2State().selectedGidIndex; }

uint64_t v2InitGeneration() { return v2State().initGeneration; }

InterfaceSnapshot v2InterfaceSnapshot() {
  const V2State& st = v2State();
  InterfaceSnapshot out;
  out.nic = st.nicName;
  out.port = st.selectedPort;
  out.gidIndex = st.selectedGidIndex;
  out.generation = st.initGeneration;
  return out;
}

int v2Init(hipObjConfigV2_t* config) {
  V2State& st = v2State();
  if (st.initialized) {
    return hipObjAlreadyInitialized;
  }
  if (hipObj::getState().initialized) {
    return hipObjAlreadyInitialized;
  }
  if (!config || !config->control.controlEndpoint ||
      config->control.controlEndpoint[0] == '\0') {
    return hipObjInvalidValue;
  }
  if (!ibv.is_initialized) {
    return hipObjRdmaError;
  }
  /* gpuDevice < 0 (auto-select) was resolved by hipObjInitV2 before
   * calling v2Init; a negative value here means the lookup failed. */
  const int gpuDevice = config->v1.gpuDevice;
  if (gpuDevice < 0) {
    return hipObjRdmaError;
  }
  const char* devName = nullptr;
  int nicIndex = hipObj::GetClosestNicToGpuSafe(
    gpuDevice, config->v1.nicHint ? config->v1.nicHint : nullptr, &devName);
  if (nicIndex < 0) {
    if (config->v1.nicHint && config->v1.nicHint[0] != '\0') {
      devName = config->v1.nicHint;
      nicIndex = 0;
    } else {
      return hipObjNicNotFound;
    }
  }
  /* All allocating state is prepared before the device opens, so a
   * throw or failure leaves nothing to roll back and a retry starts
   * from the same clean state. */
  std::string nicNameOut = (devName != nullptr) ? devName : "";
  std::string controlEndpoint = config->control.controlEndpoint;
  DeviceHandle* dh = new DeviceHandle();
  RcConnection raw;
  int ret = -1;
  try {
    /* The open helpers guarantee no exceptions escape (GID selection
     * and enumeration are wrapped), so the return-code cleanup below
     * is the only failure path. */
    ret = (devName != nullptr)
            ? hipObj::openRdmaDeviceByName(devName, raw)
            : hipObj::openRdmaDevice(nicIndex, raw);
  } catch (...) {
    ret = -1;
  }
  if (ret != 0) {
    delete dh;
    return hipObjRdmaError;
  }
  dh->ctx = raw.ctx;
  dh->pd = raw.pd;
  dh->portNum = raw.portNum;
  dh->gidIndex = raw.gidIndex;
  v2State().selectedPort = static_cast<int>(raw.portNum);
  v2State().selectedGidIndex = static_cast<int>(raw.gidIndex);
  dh->localGid = raw.localGid;
  st.device = dh;
  st.nicName = std::move(nicNameOut);
  st.controlEndpoint = std::move(controlEndpoint);
  st.connectDeadlineMs = config->connectDeadlineMs;
  st.transferDeadlineMs = config->transferDeadlineMs;
  st.cancelBudgetMs = config->cancelCleanupBudgetMs;
  ++st.initGeneration;
  st.initialized = true;
  return hipObjSuccess;
}

struct ibv_pd* v2ProtectionDomain() {
  V2State& st = v2State();
  return st.initialized ? st.device->pd : nullptr;
}

int v2Shutdown() {
  V2State& st = v2State();
  if (!st.initialized) {
    return hipObjSuccess;
  }
  ConnectionRegistry& reg = registry();
  bool poisonLeft = false;
  std::vector<ConnId> ids;
  reg.forEachId([&ids](ConnId id) { ids.push_back(id); });
  for (auto id : ids) {
    /* Drain settles each entry's MR pin only when its connection was
     * actually reclaimed. A failed release keeps the entry alive
     * (shutdown is a callable recovery API, not process exit), so the
     * pin must stay with the entry: unpinning memory a surviving QP
     * can still DMA through would let a later deregistration free it.
     * Take-before-release / restore-on-failure mirrors the ordinary
     * releaseSession policy. */
    void* entryPin = nullptr;
    bool holdsDeviceRef = true;
    reg.withEntry(id, [&](ConnectionEntryV2& e) {
      entryPin = e.pinnedBuffer;
      holdsDeviceRef = e.holdsDeviceRef;
      e.pinnedBuffer = nullptr;
      e.holdsDeviceRef = false;
    });
    const int rc = releaseConnection(id);
    if (rc == kReleaseOk) {
      if (entryPin != nullptr) {
        g_bufferMap.releaseMrRef(entryPin);
      }
      if (holdsDeviceRef) {
        releaseDevice(st.device);
      }
    } else {
      reg.withEntry(id, [&](ConnectionEntryV2& e) {
        e.pinnedBuffer = entryPin;
        e.holdsDeviceRef = holdsDeviceRef;
      });
      if (rc == kReleaseLeftover) {
        poisonLeft = true;
      }
    }
  }
  /* Attempt emergency-slot recovery even when ordinary entries
   * survive: their poison must not starve independently recoverable
   * objects. */
  drainEmergencySlots();
  if (poisonLeft || reg.size() > 0) {
    return hipObjRdmaError;
  }
  /* Retry any survivor the registry could not take: emergency slots
   * left over from insertion-time allocation failures. A slot that
   * still holds a surviving CQ must keep the device open; shutdown
   * reports failure and a later shutdown retries. */
  drainEmergencySlots();
  for (auto& slot : g_emergencySlots) {
    if (slot.used) {
      return hipObjRdmaError;
    }
  }
  /* Buffers go before the device: every MR must be deregistered
   * while the protection domain is still alive. */
  g_bufferMap.deregisterAll();
  if (st.device != nullptr) {
    RcConnection raw;
    raw.ctx = st.device->ctx;
    raw.pd = st.device->pd;
    raw.portNum = st.device->portNum;
    raw.gidIndex = st.device->gidIndex;
    raw.localGid = st.device->localGid;
    hipObj::closeRdmaDevice(raw);
    delete st.device;
    st.device = nullptr;
  }
  st.initialized = false;
  st.controlEndpoint.clear();
  st.nicName.clear();
  st.selectedPort = 0;
  st.selectedGidIndex = -1;
  ++st.initGeneration;
  return hipObjSuccess;
}

int v2Transfer(int isPut, const char* bucket, const char* key, void* devPtr,
               uint64_t size, uint64_t offset, const char* query,
               hipObjOpsV2_t* ops, void* ctx, uint64_t entryMs,
               bool haveEntryMs, uint64_t snapshotGeneration,
               bool haveSnapshot, int* diagOut) {
  V2State& st = v2State();
  if (!st.initialized) {
    return hipObjNotInitialized;
  }
  /* Publish the active interface selection on every callback request:
   * read here (under the API lock held by the public entry points) it
   * is coherent with the generation admission already validated. */
  const std::string activeNic = st.nicName;
  const int activePort = st.selectedPort;
  const int activeGid = st.selectedGidIndex;
  if (haveSnapshot && snapshotGeneration != st.initGeneration) {
    /* The caller captured its interface selection before waiting for
     * the API lock and a shutdown/reinit happened in between: the
     * snapshot mixes initializations and must not drive a transfer
     * on the new device. */
    return hipObjNotInitialized;
  }
  if (!ops || !ops->sendPrepare || !ops->sendReadyRequest ||
      !ops->finishReady || !ops->sendCancel || !bucket || !key || !devPtr) {
    return hipObjInvalidValue;
  }
  if (size == 0 || size > kMaxTransferSize) {
    return hipObjSizeTooLarge;
  }
  struct ibv_mr* mr = g_bufferMap.lookupMr(devPtr);
  if (mr == nullptr) {
    return hipObjBufNotRegistered;
  }
  if (!g_bufferMap.isDeviceBacked(devPtr)) {
    /* A host-substituted registration covers different memory than
     * the advertised device pointer; v2 must not DMA through it. */
    return hipObjBufNotRegistered;
  }
  if (g_bufferMap.lookupSize(devPtr) < size) {
    return hipObjSizeTooLarge;
  }

  /* Whole-transfer budget: entry to return, lock/admission wait
   * included when the public entry point captured its timestamp
   * before locking; otherwise this is the internal/test path and
   * v2Transfer stamps its own entry with the same injectable
   * clock, keeping deadline tests deterministic either way. */
  const uint64_t startMs = haveEntryMs ? entryMs : steadyNowMs();
  const uint32_t budgetMs = st.transferDeadlineMs != 0
                              ? st.transferDeadlineMs
                              : kDefaultTransferDeadlineMs;
  const uint64_t deadline = startMs + budgetMs;
  if (steadyNowMs() >= deadline) {
    /* The whole budget was consumed before admission completed. */
    if (diagOut != nullptr) {
      *diagOut = kDiagDeadlineExpired;
    }
    return hipObjBusy;
  }
  const uint32_t cancelBudgetMs = st.cancelBudgetMs != 0
                                    ? st.cancelBudgetMs
                                    : kDefaultCancelBudgetMs;

  hipObjError_t outcome = {hipObjInternalError, 0};
  /* Endpoint view over the stored control endpoint string; the string
   * lives in v2State() for the process lifetime of the init, so the
   * pointer stays valid across every callback. */
  hipObjControlEndpointV2_t epV2{st.controlEndpoint.c_str()};
  SessionResources res;
  Phase p = Phase::Idle;
  bool wireCancelEligible = false; /* session published on the wire */
  bool cancelSent = false;         /* single CANCEL attempt, even on throw */
  std::string sessionId;

  const auto cleanup = [&](hipObjError_t wireOutcome) -> int {
    fail(p, !exposed(p));
    /* Post-expiry cancel: one bounded attempt once a session was
     * published. PREPARE-phase expiry has nothing to cancel (the
     * parser requires a 32-hex session). */
    if (wireCancelEligible && !sessionId.empty() && !cancelSent &&
        wireOutcome.opError != hipObjSuccess) {
      hipObjTransferReqV2_t creq;
      fillCommonRequest(creq, isPut ? "PUT" : "GET", bucket, key, size,
                        offset, query, 0, 0, deadline,
                        cancelBudgetMs);
      creq.session = sessionId.c_str();
      creq.target = nullptr;
      creq.endpoint = &epV2;
      creq.nic = activeNic.c_str();
      creq.nicPort = activePort;
      creq.nicGidIndex = activeGid;
      cancelSent = true; /* single attempt even if it throws below */
      try {
        ops->sendCancel(ctx, &creq);
      } catch (...) {
        /* Best-effort wire cleanup must not bypass local release. */
      }
    }
    const int rc = releaseSession(res);
    const hipObjError_t fin0 = finalizeOutcome(wireOutcome, rc);
    if (diagOut != nullptr) {
      /* fin0 carries the deadline marker through the release-failure
       * override, so surface it unconditionally when present. */
      *diagOut = fin0.hipError == kDiagDeadlineExpired ? kDiagDeadlineExpired
                                                       : 0;
    }
    return fin0.opError;
  };

  try {
    do {
    if (!beginNegotiate(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }

    /* ---- verbs setup (per-transfer QP/CQ, MR pin) ---- */
    const size_t emergencyHandle = reserveEmergencySlot();
    if (emergencyHandle == 0) {
      /* All emergency recovery slots are outstanding: refuse the
       * transfer rather than create verbs objects whose parking
       * failure would strand them. */
      outcome = {hipObjBusy, 0};
      break;
    }
    const int arc = acquireSession(devPtr, res, emergencyHandle);
    releaseEmergencyReservation(emergencyHandle);
    if (arc != 0) {
      /* acquireSession rolled back its own QP/CQ/slot work on every
       * failure path, but the MR pin survives in res.pinnedBuffer
       * (and partially destroyed verbs objects survive when the
       * rollback itself failed, arc == -2). Do NOT erase res here:
       * releaseSession must see exactly what was and was not
       * acquired so leftovers are reported, not leaked. */
      outcome = arc == -2 ? hipObjError_t{hipObjInternalError, 0}
                          : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    uint32_t psn = 0;
    if (!nextClientPsn(psn) || psn == 0) {
      /* Random-source failure must not silently reuse a fixed PSN:
       * fail closed before anything is published on the wire. */
      outcome = {hipObjInternalError, 0};
      break;
    }
    res.conn.qpNum = res.conn.qp != nullptr ? res.conn.qp->qp_num : 0;
    {
      ConnectionRegistry& reg = registry();
      /* Reuse guard: the (qpn, psn) pair must not collide with a
       * retired pair still inside its reuse window. Random selection
       * makes this improbable, not impossible; check before the pair
       * reaches the wire. */
      if (reg.retired().contains(res.conn.qpNum, psn)) {
        outcome = {hipObjBusy, 0};
        break;
      }
      reg.withEntry(res.id, [&](ConnectionEntryV2& entry) {
        entry.clientPsn = psn;
      });
    }

    /* Client token carries this QP's identity for the server's RTR. */
    hipObj::RdmaToken tok{};
    tok.qpNum = res.conn.qpNum;
    std::memcpy(tok.gid, &res.dh->localGid, sizeof(tok.gid));
    tok.transport = hipObj::TRANSPORT_RC;
    tok.portNum = res.dh->portNum;
    const std::string clientToken = hipObj::encodeRdmaToken(tok);

    /* ---- PREPARE ---- */
    std::string target = buildTarget(bucket, key,
                                     query != nullptr ? query : "");
    uint32_t cookie = 0;
    if (!nextCookie(cookie) || cookie == 0) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    hipObjTransferReqV2_t preq;
    fillCommonRequest(preq, isPut ? "PUT" : "GET", bucket, key, size, offset,
                      query, cookie, psn, deadline, remaining(deadline));
    preq.token = clientToken.c_str();
    preq.target = target.c_str();
    preq.endpoint = &epV2;
    preq.nic = activeNic.c_str();
    preq.nicPort = activePort;
    preq.nicGidIndex = activeGid;

    hipObjPrepareReplyV2_t prep;
    std::memset(&prep, 0, sizeof(prep));
    if (remaining(deadline) == 0) {
      /* Admit nothing new once the budget is spent. */
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    const int prc = ops->sendPrepare(ctx, &preq, &prep);
    if (prc != 0) {
      outcome = remaining(deadline) == 0
                    ? hipObjError_t{hipObjBusy, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjS3Error, 0};
      break;
    }
    if (remaining(deadline) == 0) {
      /* The callback consumed the whole budget: even a well-formed
       * reply must not advance the protocol. */
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    if (prep.httpStatus == 501 && prep.unsupportedMarker) {
      outcome = {hipObjNotSupported, 0};
      break;
    }
    if (prep.httpStatus != 200) {
      outcome = prep.httpStatus == 503
                  ? hipObjError_t{hipObjBusy, 0}
                  : (prep.httpStatus == 403 || prep.httpStatus == 400
                       ? hipObjError_t{hipObjInvalidValue, 0}
                       : hipObjError_t{hipObjS3Error, 0});
      break;
    }
    if (!prepareOk(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    /* Validate the reply token: the peer transport must decode, match
     * the advertised QPN, and carry a routable GID. */
    hipObj::RdmaToken replyTok{};
    bool peerOk = false;
    uint32_t serverQpn = 0;
    union ibv_gid serverGid{};
    {
      /* serverToken is the 88-hex payload; the "200:" prefix was
       * stripped by the consumer (bridge/Go wrapper) per the wire. */
      if (prep.serverToken[0] != '\0' &&
          hipObj::decodeRdmaTokenHex(prep.serverToken, replyTok) &&
          replyTok.transport == hipObj::TRANSPORT_RC) {
        static const uint8_t kZeroGid[16] = {0};
        if (std::memcmp(replyTok.gid, kZeroGid, 16) != 0) {
          serverQpn = replyTok.qpNum;
          std::memcpy(&serverGid, replyTok.gid, 16);
          peerOk = true;
        }
      }
    }
    const bool sessionHex = [&] {
      const char* q = prep.session;
      for (size_t i = 0; i < 32; ++i) {
        if (!isxdigit(static_cast<unsigned char>(q[i]))) return false;
      }
      return q[32] == '\0';
    }();
    if (!peerOk || !sessionHex) {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    if (!prep.protocolEcho) {
      outcome = {hipObjRdmaError, 0};
      break;
    }
    if (prep.serverPsn == 0 || prep.serverPsn > 0xffffffu) {
      /* The wire contract requires a nonzero 24-bit server PSN;
       * inventing one would desynchronize the RC stream. */
      outcome = {hipObjRdmaError, 0};
      break;
    }
    sessionId = prep.session;
    wireCancelEligible = true;
    const uint32_t serverPsn = prep.serverPsn;
    /* PUT requires a valid staging advertisement: both the address
     * and a nonzero remote key (zero documents absence). */
    if (isPut && (!prep.stagingPresent || prep.stagingAddr == 0 ||
                  prep.stagingRkey == 0)) {
      outcome = {hipObjInvalidValue, 0};
      break;
    }

    /* ---- QP RTR/RTS, bounded by the configured connect deadline -- */
    const uint64_t connectBudgetMs =
      st.connectDeadlineMs != 0 ? st.connectDeadlineMs
                                : kDefaultConnectDeadlineMs;
    const uint64_t nowMs0 = steadyNowMs();
    const uint64_t connectDeadline =
      nowMs0 + connectBudgetMs < deadline ? nowMs0 + connectBudgetMs
                                          : deadline;
    if (steadyNowMs() >= connectDeadline) {
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    if (transitionQpToRtrV2(res.dh, res.conn, serverQpn, 0, serverGid,
                            serverPsn) != 0) {
      /* A failed transition may also have consumed the allowance;
       * sample the deadline fact so it survives classification and
       * release precedence. */
      const bool rtrExpired = steadyNowMs() >= connectDeadline;
      outcome = rtrExpired
                    ? hipObjError_t{hipObjRdmaError, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    if (steadyNowMs() >= connectDeadline) {
      /* RTR consumed the allowance: do not start RTS with an expired
       * budget. */
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    if (transitionQpToRtsV2(res.conn, res.dh, psn) != 0) {
      const bool rtsExpired = steadyNowMs() >= connectDeadline;
      outcome = rtsExpired
                    ? hipObjError_t{hipObjRdmaError, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    if (steadyNowMs() >= connectDeadline) {
      /* The connect budget expired during the transitions; the pair
       * state is uncertain, so classify as expiry, not a verbs
       * failure. */
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    if (!connectOk(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }

    /* ---- GET receive posted before READY: READY arms the server's
     * WRITE_WITH_IMM, so the receive must already be in place or a
     * fast peer burns RNR retries (approved timeline). ---- */
    struct ibv_recv_wr recvWr = {};
    struct ibv_sge recvSge = {};
    if (!isPut) {
      recvSge.addr = reinterpret_cast<uintptr_t>(devPtr);
      recvSge.length = static_cast<uint32_t>(size);
      recvSge.lkey = mr->lkey;
      recvWr.sg_list = &recvSge;
      recvWr.num_sge = 1;
      struct ibv_recv_wr* bad = nullptr;
      if (ibv.post_recv(res.conn.qp, &recvWr, &bad) != 0) {
        outcome = {hipObjRdmaError, 0};
        break;
      }
    }

    /* ---- READY request (bytes out; response stays pending) ---- */
    if (!sendReady(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    hipObjTransferReqV2_t rreq;
    /* Fill with the budget actually remaining for the exchange + the
     * FINAL read, so the consumer never sees a stale allowance. */
    fillCommonRequest(rreq, isPut ? "PUT" : "GET", bucket, key, size, offset,
                      query, cookie, psn, deadline, remaining(deadline));
    rreq.session = sessionId.c_str();
    rreq.token = clientToken.c_str();
    rreq.endpoint = &epV2;
    rreq.nic = activeNic.c_str();
    rreq.nicPort = activePort;
    rreq.nicGidIndex = activeGid;
    rreq.clientQpn = res.conn.qpNum;
    rreq.clientMrAddr = reinterpret_cast<uint64_t>(devPtr);
    rreq.clientMrRkey = mr->rkey;
    if (remaining(deadline) == 0) {
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    if (ops->sendReadyRequest(ctx, &rreq) != 0) {
      outcome = remaining(deadline) == 0
                    ? hipObjError_t{hipObjBusy, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjS3Error, 0};
      break;
    }

    /* ---- data phase while the exchange is pending ---- */
    bool dataOk = false;
    struct ibv_wc wc = {};
    if (!isPut) {
      /* GET: the receive was posted before READY; poll the server's
       * WRITE_WITH_IMM completion now. */
      dataOk = pollDeadline(res.conn.cq, IBV_WC_RECV_RDMA_WITH_IMM,
                            deadline, wc) &&
               (wc.wc_flags & IBV_WC_WITH_IMM) != 0 &&
               ntohl(wc.imm_data) == cookie &&
               /* A posted receive length bounds what the remote may
                * write, it does not describe what it did write: a
                * short WRITE_WITH_IMM must not surface as success. */
               wc.byte_len == rreq.size;
    } else {
      /* PUT staging push: bounded grace so the server's receive post
       * (inside its READY handling) is not raced, then WRITE WITH
       * IMM to the staging MR. */
      boundedSleep(steadyNowMs() + kPutGraceMs, deadline);
      if (steadyNowMs() >= deadline) {
        /* Classify through the common data-expiry mapping, not the
         * loop's InternalError default. */
        outcome = {hipObjBusy, kDiagDeadlineExpired};
        break;
      }
      struct ibv_sge sge = {};
      sge.addr = reinterpret_cast<uintptr_t>(devPtr);
      sge.length = static_cast<uint32_t>(size);
      sge.lkey = mr->lkey;
      struct ibv_send_wr wr = {};
      wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      wr.send_flags = IBV_SEND_SIGNALED;
      wr.imm_data = htonl(cookie);
      wr.wr.rdma.remote_addr = prep.stagingAddr;
      wr.wr.rdma.rkey = prep.stagingRkey;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      struct ibv_send_wr* bad = nullptr;
      if (ibv.post_send(res.conn.qp, &wr, &bad) == 0) {
        dataOk = pollDeadline(res.conn.cq, IBV_WC_RDMA_WRITE, deadline, wc);
      }
    }
    if (!dataOk) {
      outcome = remaining(deadline) == 0
                    ? hipObjError_t{hipObjBusy, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjRdmaError, 0};
      break;
    }

    /* ---- finishReady: consume the pending FINAL response ---- */
    hipObjFinalReplyV2_t fin;
    std::memset(&fin, 0, sizeof(fin));
    if (remaining(deadline) == 0) {
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    /* Refresh the budget fields right before FINAL: the earlier fill
     * reflects the pre-READY allowance, and the bridge must not spend
     * a stale portion on this read. */
    rreq.deadlineMs = static_cast<uint32_t>(
      remaining(deadline) > 0xffffffff ? 0xffffffff : remaining(deadline));
    rreq.remainingMs = rreq.deadlineMs;
    if (ops->finishReady(ctx, &rreq, &fin) != 0) {
      outcome = remaining(deadline) == 0
                    ? hipObjError_t{hipObjBusy, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjS3Error, 0};
      break;
    }
    /* Sample the expiry fact before classifying the reply: an expired
     * FINAL whose answer is also invalid must still surface the
     * deadline, because the timeout is the actionable diagnosis and
     * teardown precedence would otherwise drop it. */
    const uint32_t finalExpired = remaining(deadline) == 0;
    const bool finalOk = fin.httpStatus == 200 ||
                         (isPut && fin.httpStatus == 204);
    if (!finalOk) {
      outcome = finalExpired ? hipObjError_t{hipObjS3Error, kDiagDeadlineExpired}
                             : hipObjError_t{hipObjS3Error, 0};
      break;
    }
    if (!fin.cookiePresent || fin.cookieEcho != cookie) {
      outcome = finalExpired
                    ? hipObjError_t{hipObjRdmaError, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    if (!fin.protocolEcho) {
      /* The wire parser rejects successful replies without the echo;
       * hold the core to the same contract. */
      outcome = finalExpired
                    ? hipObjError_t{hipObjRdmaError, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    if (fin.bytes != rreq.size) {
      /* The server's own accounting must agree with the request; a
       * mismatch means a short or over-long transfer never surfaced
       * as a data-phase error. */
      outcome = finalExpired
                    ? hipObjError_t{hipObjRdmaError, kDiagDeadlineExpired}
                    : hipObjError_t{hipObjRdmaError, 0};
      break;
    }
    if (!transferDone(p)) {
      outcome = {hipObjInternalError, 0};
      break;
    }
    if (remaining(deadline) == 0) {
      /* The transfer completed on the wire but outside its budget:
       * report expiry, not success. */
      outcome = {hipObjBusy, kDiagDeadlineExpired};
      break;
    }
    outcome = HIPOBJ_SUCCESS;
    } while (false);
    return cleanup(outcome);
  } catch (...) {
    /* An allocation failure mid-transfer must not bypass cleanup:
     * release what was acquired so leftovers are reported, then map
     * to the internal-error classification. */
    return cleanup(hipObjError_t{hipObjInternalError, 0});
  }
}

} // namespace v2
} // namespace hipObj
