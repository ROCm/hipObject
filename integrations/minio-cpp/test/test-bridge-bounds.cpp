/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Bounds on the minio-cpp bridge: outstanding DNS workers stay
 * capped, a failed thread launch is a transport failure, and a
 * successful credential fetch is reused for the rest of a transfer.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <miniocpp/credentials.h>
#include <miniocpp/providers.h>
#include <netdb.h>

#include "hipobj_minio/context.h"
#include "hipobj_minio/rdma.h"

namespace {

using hipobj::minio::S3RdmaContext;
using hipobj::minio::test::connectControlForTest;
using hipobj::minio::test::fetchCredsForTest;
using hipobj::minio::test::outstandingResolverCount;
using hipobj::minio::test::resetOutstandingResolvers;
using hipobj::minio::test::setForceResolverLaunchFail;
using hipobj::minio::test::setResolveHook;

class CountingProvider : public minio::creds::Provider {
public:
  std::atomic<int> fetches{0};
  minio::creds::Credentials Fetch() override {
    fetches.fetch_add(1);
    return minio::creds::Credentials(minio::error::Error(), "AK", "SK");
  }
};

int stallResolve(const char*, const char*, const struct addrinfo*,
                 struct addrinfo** res) {
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  if (res) {
    *res = nullptr;
  }
  return EAI_FAIL;
}

class BridgeBoundsTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetOutstandingResolvers();
    setResolveHook(nullptr);
    setForceResolverLaunchFail(false);
  }
  void TearDown() override {
    setResolveHook(nullptr);
    setForceResolverLaunchFail(false);
    resetOutstandingResolvers();
  }
};

} // namespace

TEST_F(BridgeBoundsTest, CredentialFetchIsCached) {
  CountingProvider provider;
  S3RdmaContext ctx{};
  ctx.provider = &provider;
  auto a = fetchCredsForTest(&ctx);
  auto b = fetchCredsForTest(&ctx);
  EXPECT_EQ(provider.fetches.load(), 1);
  EXPECT_EQ(a.access_key, "AK");
  EXPECT_EQ(b.access_key, "AK");
}

TEST_F(BridgeBoundsTest, ResolverLaunchFailureIsTransportFailure) {
  setForceResolverLaunchFail(true);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  EXPECT_FALSE(connectControlForTest("127.0.0.1:9", deadline));
  EXPECT_EQ(outstandingResolverCount(), 0u);
}

TEST_F(BridgeBoundsTest, OutstandingResolversStayCapped) {
  setResolveHook(stallResolve);
  constexpr int kLaunch = 8;
  std::mutex mu;
  std::condition_variable cv;
  int started = 0;
  std::vector<std::thread> workers;
  workers.reserve(kLaunch);
  for (int i = 0; i < kLaunch; ++i) {
    workers.emplace_back([&]() {
      {
        std::lock_guard<std::mutex> lk(mu);
        ++started;
        cv.notify_all();
      }
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(250);
      (void)connectControlForTest("stalled.invalid:9", deadline);
    });
  }
  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::seconds(2), [&]() {
      return started == kLaunch;
    });
  }
  /* The stall hook holds a slot for 400 ms; the connect deadline is
   * 250 ms, so each call detaches and leaves the worker counted
   * until the hook returns. The cap is 4. */
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  const unsigned outstanding = outstandingResolverCount();
  EXPECT_LE(outstanding, 4u);
  EXPECT_GT(outstanding, 0u);
  for (auto& t : workers) {
    t.join();
  }
  /* Give detached helpers time to release their slots. */
  const auto giveUp = std::chrono::steady_clock::now() +
                      std::chrono::seconds(2);
  while (outstandingResolverCount() != 0u &&
         std::chrono::steady_clock::now() < giveUp) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(outstandingResolverCount(), 0u);
}
