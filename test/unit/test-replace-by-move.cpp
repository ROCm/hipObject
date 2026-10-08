/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for replaceByMove(): move assignment over an object that
 * still owns its members releases them in the destructor's order.
 */

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hipobj-warnings.h"
#include "replace-by-move.h"

/* Google Test registers each test with a global constructor */
HIPOBJ_WARN_NO_GLOBAL_CTOR_OFF

namespace {

using Log = std::vector<std::string>;

/* Logs its name when it is destroyed while it still owns something. A
 * moved-from Probe owns nothing and logs nothing. */
class Probe {
public:
    Probe(Log *log, std::string name) : log_(log), name_(std::move(name))
    {
    }
    ~Probe()
    {
        if (log_ != nullptr) {
            log_->push_back(name_);
        }
    }

    Probe(const Probe &)            = delete;
    Probe &operator=(const Probe &) = delete;
    Probe(Probe &&other) noexcept : log_(std::exchange(other.log_, nullptr)), name_(std::move(other.name_))
    {
    }
    Probe &operator=(Probe &&) = delete;

    const std::string &name() const
    {
        return name_;
    }

private:
    Log        *log_;
    std::string name_;
};

/* Declared like RcConnV2: second depends on first, so the destructor
 * releases second first. */
struct Owner final {
    Probe first;
    Probe second;

    Owner(Log *log, const std::string &tag) : first(log, tag + ".first"), second(log, tag + ".second")
    {
    }
    ~Owner()                        = default;
    Owner(const Owner &)            = delete;
    Owner &operator=(const Owner &) = delete;
    Owner(Owner &&) noexcept        = default;
    Owner &operator=(Owner &&other) noexcept
    {
        hipObj::replaceByMove(*this, std::move(other));
        return *this;
    }
};

} // namespace

/* Assigning over a populated object releases its members in reverse
 * declaration order, then takes over the source's members. The
 * moved-from source releases nothing when it is destroyed. */
TEST(ReplaceByMove, ReleasesTargetInDestructorOrder)
{
    Log log;
    {
        Owner target(&log, "target");
        Owner source(&log, "source");

        target = std::move(source);

        EXPECT_EQ(log, (Log{"target.second", "target.first"}));
        EXPECT_EQ(target.first.name(), "source.first");
        EXPECT_EQ(target.second.name(), "source.second");
    }
    EXPECT_EQ(log, (Log{"target.second", "target.first", "source.second", "source.first"}));
}

/* Self-move assignment leaves the object as it was */
TEST(ReplaceByMove, SelfMoveKeepsMembers)
{
    Log log;
    {
        Owner  obj(&log, "obj");
        Owner &self = obj;

        obj = std::move(self);

        EXPECT_TRUE(log.empty());
        EXPECT_EQ(obj.first.name(), "obj.first");
        EXPECT_EQ(obj.second.name(), "obj.second");
    }
    EXPECT_EQ(log, (Log{"obj.second", "obj.first"}));
}
