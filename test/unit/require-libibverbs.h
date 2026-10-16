/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Whether the tests that load the system's libibverbs have to load it.
 * They skip when it doesn't load, since a system can lack libibverbs.
 * Setting HIPOBJ_TEST_REQUIRE_LIBIBVERBS to 1 makes them fail instead.
 * CI sets it, since its images all have libibverbs, so the tests can't
 * pass there by skipping. CI's build workflow also runs them with only
 * libibverbs' runtime package installed, which checks that hipObject
 * doesn't need the development package's libibverbs.so link.
 */

#pragma once

#include <cstdlib>
#include <string_view>

namespace hipObjTest {

/* Use it where the test would skip:
 *
 *     ASSERT_FALSE(libibverbsRequired()) << "libibverbs couldn't be loaded";
 *     GTEST_SKIP() << "libibverbs couldn't be loaded";
 */
inline bool
libibverbsRequired()
{
    const char *value = std::getenv("HIPOBJ_TEST_REQUIRE_LIBIBVERBS");
    return value != nullptr && std::string_view(value) == "1";
}

} // namespace hipObjTest
