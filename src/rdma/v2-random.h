/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Injectable randomness for v2 policies (client PSNs, completion
 * cookies). Production draws from getrandom(2); unit tests install
 * a deterministic source. */

#pragma once

#include <cstdint>
#include <optional>

namespace hipObj {
namespace v2 {

    /* The methods are const, so one source can be shared by everything
     * that uses it */
    class RandomSource {
    public:
        RandomSource()          = default;
        virtual ~RandomSource() = default;

        /* Copying through a reference to the base would slice */
        RandomSource(const RandomSource &)            = delete;
        RandomSource &operator=(const RandomSource &) = delete;
        RandomSource(RandomSource &&)                 = delete;
        RandomSource &operator=(RandomSource &&)      = delete;

        /* Draws 32 random bits. Returns std::nullopt only when the
         * underlying entropy source failed; callers treat that as an
         * internal error rather than falling back to a constant. */
        virtual std::optional<uint32_t> next32() const = 0;
    };

    /* Returns the active source (production default unless a test
     * override is installed). */
    const RandomSource &randomSource();

    /* Installs a test source and returns the previously active one
     * (nullptr when the production default was active). Passing
     * nullptr restores the default. Unit tests only. */
    const RandomSource *setRandomSourceForTest(const RandomSource *source);

    /* Draws a 24-bit non-zero PSN. Retries up to twice when the draw
     * masks to zero; returns std::nullopt when the source failed or
     * every draw was zero. */
    std::optional<uint32_t> nextClientPsn();

    /* Draws a full 32-bit completion cookie (0 is a valid cookie), or
     * returns std::nullopt when the source failed. */
    std::optional<uint32_t> nextCookie();

} // namespace v2
} // namespace hipObj
