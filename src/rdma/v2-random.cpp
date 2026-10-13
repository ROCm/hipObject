/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys Inc. and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2-random.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <sys/random.h>
#include <sys/types.h>

namespace hipObj {
namespace v2 {

    namespace {

        class GetrandomSource : public RandomSource {
        public:
            std::optional<uint32_t> next32() override
            {
                /* Loop over short reads and EINTR; fail closed on hard errors. */
                size_t                                      filled = 0;
                std::array<unsigned char, sizeof(uint32_t)> buf{};
                while (filled < buf.size()) {
                    const std::span<unsigned char> rest = std::span(buf).subspan(filled);
                    ssize_t                        n    = ::getrandom(rest.data(), rest.size(), 0);
                    if (n < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        return std::nullopt;
                    }
                    if (n == 0) {
                        return std::nullopt;
                    }
                    filled += static_cast<size_t>(n);
                }
                uint32_t value = 0;
                for (const unsigned char byte : buf) {
                    value = (value << 8) | byte;
                }
                return value;
            }
        };

        GetrandomSource g_defaultSource;
        RandomSource   *g_override = nullptr;

    } // namespace

    RandomSource &randomSource()
    {
        return g_override ? *g_override : static_cast<RandomSource &>(g_defaultSource);
    }

    RandomSource *setRandomSourceForTest(RandomSource *source)
    {
        RandomSource *previous = g_override;
        g_override             = source;
        return previous;
    }

    std::optional<uint32_t> nextClientPsn()
    {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto value = randomSource().next32();
            if (!value) {
                return std::nullopt;
            }
            const uint32_t psn = *value & 0x00ffffff; /* PSNs are 24-bit on the wire */
            if (psn != 0) {
                return psn;
            }
        }
        return std::nullopt;
    }

    std::optional<uint32_t> nextCookie()
    {
        return randomSource().next32();
    }

} // namespace v2
} // namespace hipObj
