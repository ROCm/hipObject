/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * A fake RDMA device and HIP runtime for tests of the public API. The
 * fixture substitutes fakes for libibverbs, the HIP runtime, NIC
 * enumeration, and the driver state, so hipObjInit() and transfers run
 * without GPU or RDMA hardware. The fakes record their calls and can be
 * told to fail (see FakeDevice).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <hip/hip_runtime_api.h>

#include "hip-seam.h"
#include "hipobj.h"
#include "ibv-core.h"
#include "ibv-wrapper.h"
#include "nic-seam.h"
#include "state.h"
#include "token.h"

namespace hipObjTest {

/* The calls the fakes have seen. The verbs objects are counted as they're
 * created (successfully) and destroyed, so a test can check that none
 * leaked. */
struct CallLog {
    int sendRequest = 0;
    int recvReply   = 0;
    int registerMr  = 0;
    int mrAccess    = 0; /* the access flags of the last reg_mr */
    int memcpy      = 0;
    int openDevice  = 0;
    int closeDevice = 0;
    int allocPd     = 0;
    int deallocPd   = 0;
    int createCq    = 0;
    int destroyCq   = 0;
    int createQp    = 0;
    int destroyQp   = 0;
};

/* Which fakes fail */
struct Faults {
    bool getDeviceList = false;
    bool openDevice    = false;
    bool allocPd       = false;
    bool queryPort     = false;
    bool queryGid      = false;
    bool queryDevice   = false;
    bool createCq      = false;
    bool createQp      = false;
    /* modify_qp fails for a transition to this state */
    int modifyQpToState = -1;
    /* reg_mr fails for this address, so hipObjBufRegister() falls back to a
     * host staging buffer */
    void *registerAddr = nullptr;
    /* reg_mr fails for every address, the staging buffer's too */
    bool registerAll = false;
    bool hostMalloc  = false;
};

/* One modify_qp() call */
struct ModifyQpCall {
    struct ibv_qp_attr attr = {};
    int                mask = 0;
};

/* What the fakes return, and what they record. The fixture resets it before
 * each test. Trivially destructible, so it can live in a static. */
struct FakeDevice {
    CallLog log;
    Faults  fail;

    /* HIP: the GPUs, and what the runtime reports for every pointer (its
     * memory type and the allocation it's in) */
    int           deviceCount       = 1;
    hipError_t    deviceCountErr    = hipSuccess;
    hipError_t    deviceSyncErr     = hipSuccess;
    hipMemoryType memoryType        = hipMemoryTypeDevice;
    uintptr_t     allocBase         = 0;
    size_t        allocSize         = 0;
    hipError_t    addressRangeErr   = hipSuccess;
    int           addressRangeCalls = 0;

    /* ibverbs: the vendor query_device() reports, what poll_cq() returns,
     * and the QP transitions */
    uint32_t                    vendorId      = 0;
    int                         pollResult    = 1;
    enum ibv_wc_status          pollStatus    = IBV_WC_SUCCESS;
    enum ibv_wc_opcode          pollOpcode    = IBV_WC_SEND;
    std::array<ModifyQpCall, 8> modifyQp      = {};
    size_t                      modifyQpCount = 0;

    /* The S3 server's reply to a transfer (see setReply()) */
    std::array<char, 128> reply    = {'2', '0', '0', '\0'};
    size_t                replyLen = 4;
};

FakeDevice &fake();

/* Sets the reply the fake S3 server sends, with its NUL, as the default "200"
 * is sent */
void setReply(std::string_view reply);

/* A reply that carries a peer token, which makes the transfer connect the
 * QP to the peer */
hipObj::RdmaToken peerToken(uint8_t transport = hipObj::TRANSPORT_RC);
void              setReplyWithPeerToken(const hipObj::RdmaToken &token);

/* The modify_qp() calls so far */
std::vector<ModifyQpCall> modifyQpCalls();

hipError_t fakePointerGetAttributes(hipPointerAttribute_t *attr, const void *ptr);
hipError_t fakeMemGetAddressRange(hipDeviceptr_t *base, size_t *size, hipDeviceptr_t ptr);

/* hipObjOps_t callbacks for the fake S3 server */
int         fakeSendRequest(void *ctx, const char *token, size_t tokenLen);
int         fakeRecvReply(void *ctx, char *reply, size_t *replyLen);
hipObjOps_t makeOps();

/* A fake device address. reg_mr is faked, so nothing reads or writes it. */
inline void *
fakeDevBuf()
{
    return reinterpret_cast<void *>(0x10000);
}

constexpr size_t kFakeBufSize = 64;

class EmptyNicEnumerator : public hipObj::NicEnumerator {
public:
    std::vector<hipObj::NicInfo> Enumerate(const char *) override;
};

/* Installs the fakes for each test, and restores the real functions after */
class FakeDeviceTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    static hipObjConfig_t makeConfig();

    /* Initializes the library with the fake device */
    static void init();

    /* Initializes and registers kFakeBufSize bytes at fakeDevBuf() */
    static void initAndRegister();

    hipObj::DriverState    state_;
    hipObj::DriverState   *savedState_ = nullptr;
    EmptyNicEnumerator     enumerator_;
    hipObj::NicEnumerator *savedEnumerator_ = nullptr;
    hipObj::IbvFuncs       savedFuncs_      = {};
    hipObj::HipOps         savedHipOps_;
    bool                   savedIbvInitialized_ = false;
};

} // namespace hipObjTest
