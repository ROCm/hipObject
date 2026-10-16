/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Mutexes and locks that Clang's thread safety analysis checks.
 *
 * With -Wthread-safety, Clang checks at compile time that code holds a
 * mutex wherever it reads or writes what the mutex guards, and that it
 * releases every mutex it takes. It only knows what a mutex or a lock
 * does from attributes on its declaration, and libstdc++'s mutexes and
 * locks have none, so the library uses these wrappers instead. Each
 * mutex is declared with what it guards, and each function that needs a
 * mutex held, or not held, says so:
 *
 *     mutable Mutex mutex_{};
 *     size_t        count_ HIPOBJ_GUARDED_BY(mutex_) = 0;
 *
 *     // Takes mutex_, so the caller mustn't hold it
 *     size_t count() const HIPOBJ_REQUIRES(!mutex_);
 *     // Reads count_, so the caller has to hold mutex_
 *     bool   isEmpty() const HIPOBJ_REQUIRES(mutex_);
 *
 * A function that takes a mutex that's a member of its class has to
 * declare that it requires the mutex not to be held, as count() does
 * (-Wthread-safety-negative). Its callers in the same class have to
 * declare it too, but callers outside it don't.
 *
 * The analysis has gaps, so it doesn't replace ThreadSanitizer:
 *
 * - It doesn't check constructors and destructors, which can use what a
 *   mutex guards without holding it.
 * - A lambda doesn't know what its caller holds, so it can't use what
 *   a mutex guards. That's why ConditionVariable::wait() takes no
 *   predicate: the caller checks what it's waiting for in a loop.
 * - Calling a member function of a guarded variable, such as
 *   std::map::erase(), only counts as reading it, so a shared lock is
 *   enough for the analysis even when the function changes it.
 * - It identifies a mutex by the expression that names it, so it
 *   doesn't know when two pointers or references refer to the same
 *   one.
 *
 * https://clang.llvm.org/docs/ThreadSafetyAnalysis.html describes the
 * attributes. GCC doesn't have them, so the macros are empty there, and
 * the wrappers are only the standard library's mutexes. */

#pragma once

#include <condition_variable>
#include <mutex>
#include <shared_mutex>

/* clang-format off */
#ifdef __clang__
    #define HIPOBJ_THREAD_ANNOTATION(x) __attribute__((x))
#else
    #define HIPOBJ_THREAD_ANNOTATION(x)
#endif
/* clang-format on */

/* On a class: its objects are mutexes */
#define HIPOBJ_CAPABILITY(name) HIPOBJ_THREAD_ANNOTATION(capability(name))
/* On a class: its objects hold a mutex for as long as they live */
#define HIPOBJ_SCOPED_CAPABILITY HIPOBJ_THREAD_ANNOTATION(scoped_lockable)

/* On a member or global variable: reading it requires mutex to be held,
 * at least shared, and writing it requires mutex to be held
 * exclusively */
#define HIPOBJ_GUARDED_BY(mutex) HIPOBJ_THREAD_ANNOTATION(guarded_by(mutex))
/* On a mutex: it's never taken while one of the others is held */
#define HIPOBJ_ACQUIRED_AFTER(...) HIPOBJ_THREAD_ANNOTATION(acquired_after(__VA_ARGS__))

/* On a function: the caller has to hold the mutexes exclusively, or for
 * a negative capability, !mutex, not hold it at all */
#define HIPOBJ_REQUIRES(...) HIPOBJ_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
/* On a function: the caller has to hold the mutexes, at least shared */
#define HIPOBJ_REQUIRES_SHARED(...) HIPOBJ_THREAD_ANNOTATION(requires_shared_capability(__VA_ARGS__))
/* On a function: it takes the mutexes, exclusively or shared, and
 * returns with them held. Without arguments, on a member of a mutex, it
 * takes the mutex itself. */
#define HIPOBJ_ACQUIRE(...)        HIPOBJ_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define HIPOBJ_ACQUIRE_SHARED(...) HIPOBJ_THREAD_ANNOTATION(acquire_shared_capability(__VA_ARGS__))
/* On a function: it releases the mutexes, which the caller holds
 * exclusively, or shared. Without arguments, on a member of a mutex, it
 * releases the mutex itself, and on a member of a lock, it releases the
 * mutex the lock holds, however it holds it. */
#define HIPOBJ_RELEASE(...)        HIPOBJ_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define HIPOBJ_RELEASE_SHARED(...) HIPOBJ_THREAD_ANNOTATION(release_shared_capability(__VA_ARGS__))

namespace hipObj {

/* A std::mutex that the analysis knows is a mutex. Take it with a
 * ScopedLock. */
class HIPOBJ_CAPABILITY("mutex") Mutex {
public:
    Mutex() noexcept = default;
    ~Mutex()         = default;

    Mutex(const Mutex &)            = delete;
    Mutex &operator=(const Mutex &) = delete;
    Mutex(Mutex &&)                 = delete;
    Mutex &operator=(Mutex &&)      = delete;

    void lock() HIPOBJ_ACQUIRE()
    {
        mutex_.lock();
    }

    void unlock() HIPOBJ_RELEASE()
    {
        mutex_.unlock();
    }

    /* Lets HIPOBJ_REQUIRES(!mutex) name the negative capability. It's
     * only used in attributes, which never call it, so it isn't
     * defined. */
    const Mutex &operator!() const;

private:
    friend class ConditionVariable;

    std::mutex mutex_{};
};

/* A std::shared_mutex that the analysis knows is a mutex. Take it
 * exclusively with a ScopedLock, or shared with a SharedLock. */
class HIPOBJ_CAPABILITY("mutex") SharedMutex {
public:
    SharedMutex()  = default;
    ~SharedMutex() = default;

    SharedMutex(const SharedMutex &)            = delete;
    SharedMutex &operator=(const SharedMutex &) = delete;
    SharedMutex(SharedMutex &&)                 = delete;
    SharedMutex &operator=(SharedMutex &&)      = delete;

    void lock() HIPOBJ_ACQUIRE()
    {
        mutex_.lock();
    }

    void unlock() HIPOBJ_RELEASE()
    {
        mutex_.unlock();
    }

    void lock_shared() HIPOBJ_ACQUIRE_SHARED()
    {
        mutex_.lock_shared();
    }

    void unlock_shared() HIPOBJ_RELEASE_SHARED()
    {
        mutex_.unlock_shared();
    }

    /* As Mutex's */
    const SharedMutex &operator!() const;

private:
    std::shared_mutex mutex_{};
};

/* Holds a Mutex or a SharedMutex exclusively for as long as it lives,
 * like a std::scoped_lock of one mutex */
template <typename M> class HIPOBJ_SCOPED_CAPABILITY ScopedLock {
public:
    explicit ScopedLock(M &mutex) HIPOBJ_ACQUIRE(mutex) : mutex_(&mutex)
    {
        mutex.lock();
    }

    ~ScopedLock() HIPOBJ_RELEASE()
    {
        mutex_->unlock();
    }

    ScopedLock(const ScopedLock &)            = delete;
    ScopedLock &operator=(const ScopedLock &) = delete;
    ScopedLock(ScopedLock &&)                 = delete;
    ScopedLock &operator=(ScopedLock &&)      = delete;

private:
    M *mutex_;
};

/* Holds a SharedMutex shared for as long as it lives, like a
 * std::shared_lock that's never unlocked or moved */
class HIPOBJ_SCOPED_CAPABILITY SharedLock {
public:
    explicit SharedLock(SharedMutex &mutex) HIPOBJ_ACQUIRE_SHARED(mutex) : mutex_(&mutex)
    {
        mutex.lock_shared();
    }

    ~SharedLock() HIPOBJ_RELEASE()
    {
        mutex_->unlock_shared();
    }

    SharedLock(const SharedLock &)            = delete;
    SharedLock &operator=(const SharedLock &) = delete;
    SharedLock(SharedLock &&)                 = delete;
    SharedLock &operator=(SharedLock &&)      = delete;

private:
    SharedMutex *mutex_;
};

/* A std::condition_variable that waits with a Mutex */
class ConditionVariable {
public:
    ConditionVariable()  = default;
    ~ConditionVariable() = default;

    ConditionVariable(const ConditionVariable &)            = delete;
    ConditionVariable &operator=(const ConditionVariable &) = delete;
    ConditionVariable(ConditionVariable &&)                 = delete;
    ConditionVariable &operator=(ConditionVariable &&)      = delete;

    /* Releases mutex, waits to be notified, and takes mutex back before
     * it returns. It can also return without being notified, so call it
     * in a loop that checks what it's waiting for. To the analysis, mutex
     * is held throughout.
     *
     * noexcept, since if the wait threw, the std::unique_lock would
     * release mutex, and then so would the caller's lock. */
    void wait(Mutex &mutex) noexcept HIPOBJ_REQUIRES(mutex)
    {
        /* Adopts the mutex the caller holds, for the wait, and gives it
         * back without releasing it */
        std::unique_lock<std::mutex> lock(mutex.mutex_, std::adopt_lock);
        changed_.wait(lock);
        (void)lock.release();
    }

    void notify_one() noexcept
    {
        changed_.notify_one();
    }

    void notify_all() noexcept
    {
        changed_.notify_all();
    }

private:
    std::condition_variable changed_{};
};

} // namespace hipObj
