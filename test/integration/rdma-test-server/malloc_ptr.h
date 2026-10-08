/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Owning pointer for malloc()ed memory */

#pragma once

#include <cstdlib>
#include <memory>

namespace hipObj {

struct FreeDeleter {
    void operator()(void *ptr) const noexcept
    {
        std::free(ptr);
    }
};

using MallocPtr = std::unique_ptr<void, FreeDeleter>;

} // namespace hipObj
