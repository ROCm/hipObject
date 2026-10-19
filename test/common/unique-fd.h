/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Owning handle for a file descriptor.
 *
 * Closes the descriptor in its destructor, so it's released on every
 * return path and when an exception unwinds. Like std::unique_ptr, it's
 * move-only, and -1 means it owns nothing. Descriptors that need some
 * other close function (such as a dmabuf from HSA) don't belong here. */

#pragma once

#include <utility>

#include <unistd.h>

namespace hipObj {

class UniqueFd {
public:
    UniqueFd() noexcept = default;

    /* Takes ownership of fd. A negative fd, such as a failed socket()'s
     * result, leaves it owning nothing. */
    explicit UniqueFd(int fd) noexcept : fd_(fd)
    {
    }

    ~UniqueFd()
    {
        reset();
    }

    UniqueFd(const UniqueFd &)            = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release())
    {
    }

    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        reset(other.release());
        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

    explicit operator bool() const noexcept
    {
        return fd_ >= 0;
    }

    /* Gives up ownership without closing, and returns the fd */
    [[nodiscard]] int release() noexcept
    {
        return std::exchange(fd_, -1);
    }

    /* Closes the owned fd, if any, and takes ownership of fd. close() isn't
     * retried on EINTR: Linux releases the fd even then, so a retry could
     * close a number another thread has just been given. */
    void reset(int fd = -1) noexcept
    {
        const int old = std::exchange(fd_, fd);
        if (old >= 0) {
            (void)::close(old);
        }
    }

private:
    int fd_ = -1;
};

} // namespace hipObj
