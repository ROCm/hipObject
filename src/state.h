/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace hipObj {

struct DriverState {
    bool        initialized = false;
    int         gpuDevice   = 0;
    std::string endpoint;
    std::string region;
    std::string nicHint;
    int         nicIndex = -1;
    uint32_t    flags    = 0;
};

/* Returns the library's driver state. Tests can install a substitute
 * (setStateForTest) to observe or reset the global without touching
 * the real singleton. */
DriverState &getState();

/* Installs a state override and returns the previously installed one
 * (nullptr when the real singleton was active). Passing nullptr
 * restores the singleton. Unit tests only; production never calls it. */
DriverState *setStateForTest(DriverState *state);

/* Holds the library-wide API lock for as long as it lives. Every public
 * function documented as serialized (see the "threads" group in
 * hipobj.h) takes it on its first line and holds it for the whole call,
 * so the library runs one such call at a time. That includes every
 * function that reads or changes library state: the driver state, the
 * buffer map, the RDMA connection, and the v2 registry.
 *
 * hipObjGet() and hipObjPut() call the application's callbacks with the
 * lock held, and the lock isn't recursive, so a callback that called a
 * locked function would deadlock. The guard detects that instead: when
 * the calling thread already holds the lock, it doesn't lock, and owns()
 * returns false. */
class ApiGuard final {
public:
    ApiGuard();
    ~ApiGuard();
    ApiGuard(const ApiGuard &)            = delete;
    ApiGuard &operator=(const ApiGuard &) = delete;
    ApiGuard(ApiGuard &&)                 = delete;
    ApiGuard &operator=(ApiGuard &&)      = delete;

    /* False when the calling thread already held the lock */
    [[nodiscard]] bool owns() const noexcept;

private:
    std::unique_lock<std::mutex> lock_;
};

} // namespace hipObj
