/* SPDX-License-Identifier: MIT
 */

#include "pd-lock.h"

#include <mutex>

namespace hipObj {

namespace {

/* Guards ibverbs object-lifecycle calls (QP/CQ/MR create/destroy)
 * against whichever PD is in play across threads -- see PdLockGuard's
 * doc comment in pd-lock.h. */
std::mutex g_pdMutex;

} // namespace

PdLockGuard::PdLockGuard() {
    g_pdMutex.lock();
}

PdLockGuard::~PdLockGuard() {
    g_pdMutex.unlock();
}

} // namespace hipObj
