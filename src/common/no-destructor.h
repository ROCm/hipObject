/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Wrapper for global state that must never be destroyed.
 *
 * The library's long-lived globals own ibverbs and HIP resources through
 * smart pointers. If they were ordinary globals, process exit would run
 * those deleters from static destructors, in no defined order relative to
 * the ibv wrapper in another translation unit (which may already have
 * dlclose()d libibverbs). hipObjShutdown() is the teardown path; at exit
 * the resources are simply left to the kernel. */

#pragma once

#include <type_traits>

namespace hipObj {

template <typename T> class NoDestructor {
public:
    NoDestructor() noexcept(std::is_nothrow_default_constructible_v<T>) : value_()
    {
    }

    /* Deliberately does not destroy value_. A union member's destructor
     * only runs if the union's owner calls it, and this one doesn't.
     * (= default is not equivalent here: it would be deleted.) */
    ~NoDestructor()
    {
    }

    NoDestructor(const NoDestructor &)            = delete;
    NoDestructor &operator=(const NoDestructor &) = delete;
    NoDestructor(NoDestructor &&)                 = delete;
    NoDestructor &operator=(NoDestructor &&)      = delete;

    T &operator*() noexcept
    {
        return value_;
    }

    T *operator->() noexcept
    {
        return &value_;
    }

private:
    union {
        T value_;
    };
};

} // namespace hipObj
