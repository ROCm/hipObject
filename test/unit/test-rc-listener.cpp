/* SPDX-License-Identifier: MIT
 */

#include <cstdlib>

#include <gtest/gtest.h>

#include "rc-listener.h"

/* RcListener::Start() now binds via RDMA CM against a real
 * struct ibv_pd/struct ibv_context, which needs real RDMA hardware --
 * this environment has none (see the hipobj-rdma-test-server
 * RDMA-availability check). What's covered here without hardware:
 * the pure port-resolution helper, and the null-pd/deviceCtx guard,
 * which returns before touching any RDMA CM API. */

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

TEST(RcListener, RejectsNullPdOrDeviceCtx) {
    int dummy = 0;
    EXPECT_EQ(hipObj::RcListener::Start(nullptr, &dummy, nullptr, 18281,
                                         nullptr, nullptr, nullptr),
              nullptr);
    EXPECT_EQ(hipObj::RcListener::Start(&dummy, nullptr, nullptr, 18281,
                                         nullptr, nullptr, nullptr),
              nullptr);
}

TEST(RcListener, StopWithNullptrIsSafe) {
    hipObj::RcListener::Stop(nullptr);
    hipObj::RcListener::Stop(nullptr);
}
