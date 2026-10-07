/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) Gluesys and Jihyeon Gim. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "v2_backend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "hipobj-format.h"

namespace hipObj {
namespace v2 {

    namespace {

        uint8_t patternByte(const std::string &target, size_t i)
        {
            uint32_t h = 2166136261U;
            for (char c : target) {
                h = (h ^ static_cast<uint8_t>(c)) * 16777619U;
            }
            return static_cast<uint8_t>((h + static_cast<uint32_t>(i)) & 0xff);
        }

        std::string pseudoEtag(std::span<const uint8_t> data)
        {
            /* Simple deterministic digest - not a real MD5; format matches
             * the 32-hex S3 ETag shape for response parsing tests. */
            uint64_t a = 1469598103934665603ULL;
            uint64_t b = 7;
            for (size_t i = 0; i < data.size(); ++i) {
                a = (a * 31 + data[i] + i) ^ (b << 3);
                b = b * 29 + data[i];
            }
            return hipObj::formatHex(a, 16) + hipObj::formatHex(b, 16);
        }

    } // namespace

    void MemoryBackend::seed(const std::string &target, size_t size)
    {
        std::scoped_lock<std::mutex> guard(mtx_);
        std::vector<uint8_t>         data(size);
        for (size_t i = 0; i < size; ++i) {
            data[i] = patternByte(target, i);
        }
        objects_[target] = std::move(data);
    }

    bool MemoryBackend::has(const std::string &target) const
    {
        std::scoped_lock<std::mutex> guard(mtx_);
        return objects_.contains(target);
    }

    bool MemoryBackend::read(const std::string &target, uint64_t offset, std::span<uint8_t> dst)
    {
        std::scoped_lock<std::mutex> guard(mtx_);
        auto                         it = objects_.find(target);
        if (it == objects_.end() || offset > it->second.size() || dst.size() > it->second.size() - offset) {
            return false;
        }
        std::ranges::copy(std::span(it->second).subspan(static_cast<size_t>(offset), dst.size()),
                          dst.begin());
        return true;
    }

    std::string MemoryBackend::write(const std::string &target, std::span<const uint8_t> data)
    {
        std::scoped_lock<std::mutex> guard(mtx_);
        std::string                  etag = pseudoEtag(data);
        objects_[target]                  = std::vector<uint8_t>(data.begin(), data.end());
        return etag;
    }

    size_t MemoryBackend::objectCount() const
    {
        std::scoped_lock<std::mutex> guard(mtx_);
        return objects_.size();
    }

} // namespace v2
} // namespace hipObj
