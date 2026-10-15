/* SPDX-License-Identifier: MIT
 */

#include <cstdlib>

#include <gtest/gtest.h>

#include "rc-listener.h"

/* RdmaCmRcListener::Start() binds via RDMA CM to a local RDMA device's address,
 * which needs real RDMA hardware -- this environment has none (see the
 * hipobj-rdma-test-server RDMA-availability check). What's covered here
 * without hardware: the pure port-resolution helper, and the missing
 * bindIp guard, which returns before touching any RDMA CM API. */

TEST(RcListenerPortFromEnv, DefaultsWhenUnset) {
    unsetenv("HIPOBJ_RC_LISTEN_PORT");
    EXPECT_EQ(hipObj::rcListenerPortFromEnv(), hipObj::kRcListenerDefaultPort);
}

TEST(RcListenerPortFromEnv, HonorsEnvOverride) {
    setenv("HIPOBJ_RC_LISTEN_PORT", "23456", 1);
    EXPECT_EQ(hipObj::rcListenerPortFromEnv(), 23456);
    unsetenv("HIPOBJ_RC_LISTEN_PORT");
}

TEST(RcListenerPortFromEnv, IgnoresInvalidValue) {
    setenv("HIPOBJ_RC_LISTEN_PORT", "not-a-port", 1);
    EXPECT_EQ(hipObj::rcListenerPortFromEnv(), hipObj::kRcListenerDefaultPort);
    unsetenv("HIPOBJ_RC_LISTEN_PORT");
}

TEST(RdmaCmRcListener, RejectsMissingBindIp) {
    EXPECT_EQ(hipObj::RdmaCmRcListener::Start(nullptr, 18281, nullptr, nullptr,
                                         nullptr),
              nullptr);
    EXPECT_EQ(hipObj::RdmaCmRcListener::Start("", 18281, nullptr, nullptr, nullptr),
              nullptr);
}

TEST(RdmaCmRcListener, StopWithNullptrIsSafe) {
    hipObj::RdmaCmRcListener::Stop(nullptr);
    hipObj::RdmaCmRcListener::Stop(nullptr);
}
