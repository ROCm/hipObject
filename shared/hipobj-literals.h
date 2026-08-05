/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* User-defined literals for byte sizes
 * Modeled after hipFile's hipfile-literals.h
 */

#pragma once

#include <cstddef>

namespace hipObj {
namespace literals {

constexpr size_t operator""_KiB(unsigned long long v) {
  return static_cast<size_t>(v) * 1024ULL;
}

constexpr size_t operator""_MiB(unsigned long long v) {
  return static_cast<size_t>(v) * 1024ULL * 1024ULL;
}

constexpr size_t operator""_GiB(unsigned long long v) {
  return static_cast<size_t>(v) * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace literals
} // namespace hipObj
