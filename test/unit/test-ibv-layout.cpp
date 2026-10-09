/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Checks that ibv-core.h, which declares libibverbs' types so that the
 * library can load libibverbs with dlopen() and build without its
 * headers, matches <infiniband/verbs.h>. A struct that doesn't match
 * would make libibverbs read and write the wrong memory, and a call
 * through ibv_context_ops (see IBVWrapper::post_send()) would call the
 * wrong function, all without a warning.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-layout.h"

namespace {

/* A struct's or union's size or alignment, a member's offset or size, or
 * a constant's value
 */
struct LayoutEntry {
    std::string_view name;
    int64_t          value;
};

#define HIPOBJ_IBV_TYPE(type)                                                                                \
    {"size of " #type, static_cast<int64_t>(sizeof(type))},                                                  \
        {"alignment of " #type, static_cast<int64_t>(alignof(type))},
#define HIPOBJ_IBV_MEMBER(type, member)                                                                      \
    {"offset of " #type "." #member, static_cast<int64_t>(offsetof(type, member))},                          \
        {"size of " #type "." #member, static_cast<int64_t>(sizeof(std::declval<type &>().member))},
#define HIPOBJ_IBV_VALUE(name) {"value of " #name, static_cast<int64_t>(name)},

/* The entries in ibv-layout-list.h, from ibv-core.h */
constexpr auto kCoreLayout = std::to_array<LayoutEntry>({
#include "ibv-layout-list.h"
});

#undef HIPOBJ_IBV_TYPE
#undef HIPOBJ_IBV_MEMBER
#undef HIPOBJ_IBV_VALUE

}

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

TEST(IbvLayout, MatchesLibibverbs)
{
    size_t                         count  = 0;
    const int64_t                 *values = hipobj_ibv_layout_real(&count);
    const std::span<const int64_t> real(values, count);

    ASSERT_EQ(real.size(), kCoreLayout.size());
    for (size_t i = 0; i < real.size(); i++) {
        EXPECT_EQ(kCoreLayout[i].value, real[i])
            << kCoreLayout[i].name << " differs between ibv-core.h and <infiniband/verbs.h>";
    }
}
