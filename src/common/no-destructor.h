/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Wrapper for global state that must never be destroyed.
 *
 * The library's long-lived globals own ibverbs and HIP resources through
 * smart pointers. If they were ordinary globals, process exit would run
 * those deleters from static destructors, which is unsafe in general:
 * they could run after a static that they depend on is destroyed.
 * hipObjShutdown() is the teardown path; at exit the resources are simply
 * left to the kernel.
 *
 * The value is built in a byte array, and nothing destroys it. The array
 * and the pointer to the value are all the class holds, so its destructor
 * is trivial, and Clang doesn't warn about a static NoDestructor with
 * -Wexit-time-destructors. */

#pragma once

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace hipObj {

template <typename T> class NoDestructor {
public:
    NoDestructor() noexcept(std::is_nothrow_default_constructible_v<T>) : value_(::new(storage_.data()) T())
    {
    }

    /* Holds value, for a T that's built by a function call */
    explicit NoDestructor(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(::new(storage_.data()) T(std::move(value)))
    {
    }

    /* Trivial: it doesn't destroy the value */
    ~NoDestructor() = default;

    NoDestructor(const NoDestructor &)            = delete;
    NoDestructor &operator=(const NoDestructor &) = delete;
    NoDestructor(NoDestructor &&)                 = delete;
    NoDestructor &operator=(NoDestructor &&)      = delete;

    T &operator*() noexcept
    {
        return *value_;
    }

    T *operator->() noexcept
    {
        return value_;
    }

private:
    /* Declared before value_, so it exists when value_ is built in it */
    alignas(T) std::array<std::byte, sizeof(T)> storage_{};
    /* The pointer that placement new returned. Getting it back from
     * storage_ instead would need a reinterpret_cast and std::launder. */
    T *value_;
};

} // namespace hipObj
