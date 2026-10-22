/*
 * Copyright (c) 2005 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2005-2014 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * librdmacm type definitions, from rdma-core's <rdma/rdma_cma.h>.
 *
 * <rdma/rdma_cma.h> includes <infiniband/verbs.h>, whose definitions
 * clash with ibv-core.h's, so the RC-CM listener uses these instead, as
 * the rest of the library uses ibv-core.h. Like ibv-core.h's ibv_pd and
 * ibv_mr, rdma_cm_id and rdma_cm_event declare only their leading
 * members, the ones hipObject reads: librdmacm allocates them, and
 * hipObject only reads them through the pointers it returns.
 */

#pragma once

#include <cstdint>

struct ibv_context;
struct ibv_qp;

enum rdma_cm_event_type {
    RDMA_CM_EVENT_ADDR_RESOLVED,
    RDMA_CM_EVENT_ADDR_ERROR,
    RDMA_CM_EVENT_ROUTE_RESOLVED,
    RDMA_CM_EVENT_ROUTE_ERROR,
    RDMA_CM_EVENT_CONNECT_REQUEST,
    RDMA_CM_EVENT_CONNECT_RESPONSE,
    RDMA_CM_EVENT_CONNECT_ERROR,
    RDMA_CM_EVENT_UNREACHABLE,
    RDMA_CM_EVENT_REJECTED,
    RDMA_CM_EVENT_ESTABLISHED,
    RDMA_CM_EVENT_DISCONNECTED,
    RDMA_CM_EVENT_DEVICE_REMOVAL,
    RDMA_CM_EVENT_MULTICAST_JOIN,
    RDMA_CM_EVENT_MULTICAST_ERROR,
    RDMA_CM_EVENT_ADDR_CHANGE,
    RDMA_CM_EVENT_TIMEWAIT_EXIT
};

enum rdma_port_space {
    RDMA_PS_IPOIB = 0x0002,
    RDMA_PS_TCP   = 0x0106,
    RDMA_PS_UDP   = 0x0111,
    RDMA_PS_IB    = 0x013F,
};

struct rdma_event_channel {
    int fd;
};

/* The leading members only (see above) */
struct rdma_cm_id {
    ibv_context        *verbs;
    rdma_event_channel *channel;
    void               *context;
    ibv_qp             *qp;
};

struct rdma_conn_param {
    const void *private_data;
    uint8_t     private_data_len;
    uint8_t     responder_resources;
    uint8_t     initiator_depth;
    uint8_t     flow_control;
    uint8_t     retry_count; /* ignored when accepting */
    uint8_t     rnr_retry_count;
    /* Fields below ignored if a QP is created on the rdma_cm_id. */
    uint8_t  srq;
    uint32_t qp_num;
};

/* The leading members only (see above) */
struct rdma_cm_event {
    rdma_cm_id        *id;
    rdma_cm_id        *listen_id;
    rdma_cm_event_type event;
    int                status;
};
