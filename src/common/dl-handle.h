/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Owning handle for a dlopen()ed library */

#pragma once

#include <memory>

#include <dlfcn.h>

namespace hipObj {

struct DlCloseDeleter {
    void operator()(void *handle) const noexcept
    {
        (void)dlclose(handle);
    }
};

using DlHandle = std::unique_ptr<void, DlCloseDeleter>;

} // namespace hipObj
