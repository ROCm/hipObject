/* SPDX-License-Identifier: MIT
 */

/* The RC-CM listener, run for real on the fake librdmacm and libibverbs
 * (see FakeRdmaCm in fake-device.h). Its accept loop runs on its own
 * thread, so the tests wait for it to get to each event. */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include <gtest/gtest.h>

#include "fake-device.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "rc-listener.h"
#include "rdmacm-core.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using hipObjTest::fake;

/* True once done() is, which the listener's thread makes it within a few
 * of its 200 ms epoll waits; false if it takes longer than 5 seconds */
bool
eventually(const std::function<bool()> &done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/* What the listener's callbacks were called with */
struct Callbacks {
    std::atomic<int> connected{0};
    std::atomic<int> disconnected{0};
};

void
onConnected(void *userCtx, void * /*qp*/)
{
    ++static_cast<Callbacks *>(userCtx)->connected;
}

void
onDisconnected(void *userCtx, void * /*qp*/)
{
    ++static_cast<Callbacks *>(userCtx)->disconnected;
}

class RcListenerTest : public hipObjTest::FakeDeviceTest {
protected:
    /* A listener on bindIp and port, through the fakes */
    [[nodiscard]] hipObj::RcListenerPtr start(const char *bindIp = "192.0.2.1",
                                              uint16_t    port   = hipObj::kRcListenerDefaultPort)
    {
        return hipObj::RcListenerPtr(hipObj::RdmaCmRcListener::Start(
            rdmaCm(), verbs(), bindIp, port, &onConnected, &onDisconnected, &callbacks_));
    }

    /* A connection the listener has accepted */
    [[nodiscard]] rdma_cm_id *connect() const
    {
        const int   accepted = fake().log.accept;
        rdma_cm_id *id       = rdmaCm().connectRequest();
        EXPECT_TRUE(eventually([&] { return fake().log.accept == accepted + 1; }));
        return id;
    }

    /* Everything the fakes created was released */
    static void expectNothingLeaked()
    {
        const hipObjTest::CallLog &log = fake().log;
        EXPECT_EQ(log.createEventChannel, log.destroyEventChannel);
        EXPECT_EQ(log.createId, log.destroyId);
        EXPECT_EQ(log.allocPd, log.deallocPd);
        EXPECT_EQ(log.createCq, log.destroyCq);
        EXPECT_EQ(log.createQp, log.destroyQp);
    }

    Callbacks callbacks_{};
};

TEST_F(RcListenerTest, StartsOnTheDeviceTheBindResolvesTo)
{
    {
        const hipObj::RcListenerPtr listener = start();
        ASSERT_NE(listener, nullptr);
        EXPECT_EQ(rdmaCm().boundAddress(), "192.0.2.1");
        EXPECT_EQ(rdmaCm().boundPort(), "18181");
        EXPECT_EQ(listener->port(), hipObj::kRcListenerDefaultPort);
        EXPECT_EQ(listener->deviceCtx(), rdmaCm().context());
        EXPECT_EQ(listener->deviceName(), "mlx5_0");
        EXPECT_NE(listener->pd(), nullptr);
        EXPECT_EQ(listener->connectionCount(), 0);
        EXPECT_EQ(fake().log.listen, 1);
        EXPECT_EQ(fake().log.allocPd, 1);
    }
    EXPECT_EQ(fake().log.deallocPd, 1);
    expectNothingLeaked();
}

TEST_F(RcListenerTest, BindsAnIpv6Address)
{
    fake().listenerAddress               = "2001:db8::1";
    const hipObj::RcListenerPtr listener = start("2001:db8::1");
    ASSERT_NE(listener, nullptr);
    EXPECT_EQ(rdmaCm().boundAddress(), "2001:db8::1");
}

/* Bound to INADDR_ANY, RDMA CM resolves no device, so there'd be no
 * context to allocate the PD on */
TEST_F(RcListenerTest, StartNeedsAnAddress)
{
    EXPECT_EQ(start(nullptr), nullptr);
    EXPECT_EQ(start(""), nullptr);
    EXPECT_EQ(fake().log.createEventChannel, 0);
}

TEST_F(RcListenerTest, StopWithNullptrIsSafe)
{
    hipObj::RdmaCmRcListener::Stop(nullptr);
    hipObj::RdmaCmRcListener::Stop(nullptr);
}

/* Each step of Start() can fail, and it releases what it made */
TEST_F(RcListenerTest, StartFailsAndReleasesWhatItMade)
{
    for (bool hipObjTest::Faults::*const fault : {
             &hipObjTest::Faults::createEventChannel,
             &hipObjTest::Faults::createId,
             &hipObjTest::Faults::bindAddr,
             &hipObjTest::Faults::bindResolvesNoDevice,
             &hipObjTest::Faults::listen,
             &hipObjTest::Faults::allocPd,
         }) {
        fake().fail        = {};
        fake().fail.*fault = true;
        EXPECT_EQ(start(), nullptr);
        expectNothingLeaked();
    }
    fake().fail = {};

    /* Not an address, and the address of no device */
    for (const char *bindIp : {"not-an-address", "::not-an-address", "192.0.2.99", "2001:db8::99"}) {
        EXPECT_EQ(start(bindIp), nullptr) << bindIp;
    }
    expectNothingLeaked();
}

/* A connection is accepted on a QP and CQ of its own, counted once it's
 * established, and torn down when it disconnects */
TEST_F(RcListenerTest, AcceptsAConnectionAndTearsItDown)
{
    {
        const hipObj::RcListenerPtr listener = start();
        ASSERT_NE(listener, nullptr);

        rdma_cm_id *id = connect();
        ASSERT_NE(id, nullptr);
        EXPECT_EQ(fake().log.createCq, 1);
        EXPECT_EQ(fake().log.createQp, 1);
        ASSERT_NE(id->qp, nullptr);
        EXPECT_EQ(id->qp->pd, listener->pd());

        rdmaCm().queueEvent(id, RDMA_CM_EVENT_ESTABLISHED);
        EXPECT_TRUE(eventually([&] { return listener->connectionCount() == 1; }));
        EXPECT_EQ(callbacks_.connected, 1);

        /* Events it doesn't handle change nothing */
        rdmaCm().queueEvent(id, RDMA_CM_EVENT_ADDR_CHANGE);
        rdmaCm().queueEvent(id, RDMA_CM_EVENT_DISCONNECTED);
        EXPECT_TRUE(eventually([] { return fake().log.destroyQp == 1; }));
        EXPECT_EQ(listener->connectionCount(), 0);
        EXPECT_EQ(callbacks_.disconnected, 1);
        EXPECT_EQ(fake().log.disconnect, 1);
        EXPECT_EQ(fake().log.destroyCq, 1);
    }
    expectNothingLeaked();
}

/* A QP, CQ or MR from one context can't be used through another, so a
 * connection that arrives on another context is rejected */
TEST_F(RcListenerTest, RejectsAConnectionOnAnotherContext)
{
    {
        const hipObj::RcListenerPtr listener = start();
        ASSERT_NE(listener, nullptr);
        ASSERT_NE(rdmaCm().connectRequest(true), nullptr);
        EXPECT_TRUE(eventually([] { return fake().log.reject == 1; }));
        EXPECT_EQ(fake().log.createCq, 0);
        EXPECT_EQ(fake().log.accept, 0);
    }
    expectNothingLeaked();
}

/* A connection the listener can't build a QP for is rejected, and one it
 * can't accept is destroyed */
TEST_F(RcListenerTest, RejectsOrDestroysAConnectionItCantSetUp)
{
    {
        const hipObj::RcListenerPtr listener = start();
        ASSERT_NE(listener, nullptr);

        fake().fail.createCq = true;
        ASSERT_NE(rdmaCm().connectRequest(), nullptr);
        EXPECT_TRUE(eventually([] { return fake().log.reject == 1; }));

        fake().fail.createCq   = false;
        fake().fail.cmCreateQp = true;
        ASSERT_NE(rdmaCm().connectRequest(), nullptr);
        EXPECT_TRUE(eventually([] { return fake().log.reject == 2; }));
        EXPECT_EQ(fake().log.destroyCq, 1);

        fake().fail.cmCreateQp = false;
        fake().fail.accept     = true;
        ASSERT_NE(rdmaCm().connectRequest(), nullptr);
        EXPECT_TRUE(eventually([] { return fake().log.destroyQp == 1; }));
        EXPECT_EQ(fake().log.destroyCq, 2);
    }
    expectNothingLeaked();
}

/* Stopping disconnects and destroys the connections still up, then frees
 * the PD */
TEST_F(RcListenerTest, StopTearsDownLiveConnections)
{
    hipObj::RcListenerPtr listener = start();
    ASSERT_NE(listener, nullptr);
    rdma_cm_id *id = connect();
    ASSERT_NE(id, nullptr);
    rdmaCm().queueEvent(id, RDMA_CM_EVENT_ESTABLISHED);
    EXPECT_TRUE(eventually([&] { return listener->connectionCount() == 1; }));

    listener.reset();
    EXPECT_EQ(fake().log.disconnect, 1);
    EXPECT_EQ(fake().log.destroyQp, 1);
    EXPECT_EQ(fake().log.deallocPd, 1);
    expectNothingLeaked();
}

/* Each async event is taken and acknowledged, one per wake-up, without
 * blocking the loop: a connection is still accepted afterwards */
TEST_F(RcListenerTest, TakesEachAsyncEvent)
{
    {
        const hipObj::RcListenerPtr listener = start();
        ASSERT_NE(listener, nullptr);
        rdmaCm().raiseAsyncEvent();
        rdmaCm().raiseAsyncEvent();
        EXPECT_TRUE(eventually([] { return fake().log.ackAsyncEvent == 2; }));
        EXPECT_NE(connect(), nullptr);
    }
    expectNothingLeaked();
}

/* An event that isn't about a QP is taken and acknowledged the same way */
TEST_F(RcListenerTest, TakesAnAsyncEventThatNamesNoQp)
{
    fake().asyncEventType = IBV_EVENT_PORT_ERR;
    {
        const hipObj::RcListenerPtr listener = start();
        ASSERT_NE(listener, nullptr);
        rdmaCm().raiseAsyncEvent();
        EXPECT_TRUE(eventually([] { return fake().log.ackAsyncEvent == 1; }));
    }
    expectNothingLeaked();
}

} // namespace
