/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* Builds the values of the entries in ibv-layout-list.h from
 * <infiniband/verbs.h>, for test-ibv-layout.cpp to compare with
 * ibv-core.h.
 *
 * This is C because it's linked with C++ code that uses ibv-core.h,
 * which defines the same structs differently, in the eyes of the
 * compiler. Two definitions of a struct in one C++ program break the
 * one definition rule, but C has no such rule.
 */

#include <stddef.h>
#include <stdint.h>

#include <infiniband/verbs.h>

#include "ibv-layout.h"

#define HIPOBJ_IBV_TYPE(type) (int64_t)sizeof(type), (int64_t) _Alignof(type),
#define HIPOBJ_IBV_MEMBER(type, member)                                                                      \
    (int64_t) offsetof(type, member), (int64_t)sizeof(((type *)0)->member),
#define HIPOBJ_IBV_VALUE(name) (int64_t)(name),

static const int64_t real_layout[] = {
#include "ibv-layout-list.h"
};

#undef HIPOBJ_IBV_TYPE
#undef HIPOBJ_IBV_MEMBER
#undef HIPOBJ_IBV_VALUE

const int64_t *
hipobj_ibv_layout_real(size_t *count)
{
    *count = sizeof(real_layout) / sizeof(real_layout[0]);
    return real_layout;
}
