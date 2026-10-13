/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "dl-handle.h"
#include "ibv-core.h"

namespace hipObj {

/* The functions the wrapper calls: libibverbs', and the two HSA calls
 * that reg_mr() makes to register a GPU buffer through a dma-buf.
 * Public so tests can build a wrapper from fakes; production code goes
 * through the wrapper methods and never touches it directly. */
struct IbvFuncs {
    struct ibv_device **(*get_device_list)(int *);
    void (*free_device_list)(struct ibv_device **);
    struct ibv_context *(*open_device)(struct ibv_device *);
    int (*close_device)(struct ibv_context *);
    const char *(*get_device_name)(struct ibv_device *);
    int (*query_device)(struct ibv_context *, struct ibv_device_attr *);
    int (*query_port)(struct ibv_context *, uint8_t, struct ibv_port_attr *);
    int (*query_gid)(struct ibv_context *, uint8_t, int, union ibv_gid *);
    struct ibv_pd *(*alloc_pd)(struct ibv_context *);
    int (*dealloc_pd)(struct ibv_pd *);
    struct ibv_mr *(*reg_dmabuf_mr)(struct ibv_pd *, uint64_t, size_t, uint64_t, int, int);
    struct ibv_mr *(*reg_mr_iova2)(struct ibv_pd *, void *, size_t, uintptr_t, int);
    int (*dereg_mr)(struct ibv_mr *);
    struct ibv_cq *(*create_cq)(struct ibv_context *, int, void *, struct ibv_comp_channel *, int);
    int (*destroy_cq)(struct ibv_cq *);
    struct ibv_qp *(*create_qp)(struct ibv_pd *, struct ibv_qp_init_attr *);
    int (*modify_qp)(struct ibv_qp *, struct ibv_qp_attr *, int);
    int (*destroy_qp)(struct ibv_qp *);
    int (*poll_cq)(struct ibv_cq *, int, struct ibv_wc *);
    int (*post_send)(struct ibv_qp *, struct ibv_send_wr *, struct ibv_send_wr **);
    int (*post_recv)(struct ibv_qp *, struct ibv_recv_wr *, struct ibv_recv_wr **);

    /* HSA, not libibverbs: hsa_amd_portable_export_dmabuf() and
     * hsa_amd_portable_close_dmabuf(). They return 0 or the failing
     * hsa_status_t as an int, so this header doesn't need the HSA
     * headers. They're here so tests can fake them, and are set whether
     * or not libibverbs loads. */
    int (*export_dmabuf)(const void *, size_t, int *, uint64_t *);
    int (*close_dmabuf)(int);
};

class IBVWrapper {
public:
    /* Loads libibverbs. is_initialized() says whether it succeeded. */
    IBVWrapper();

    /* Calls funcs, including its HSA entries, as given, and loads
     * nothing. For tests, which pass fakes; is_initialized() is true. */
    explicit IBVWrapper(const IbvFuncs &funcs);

    ~IBVWrapper() = default;

    /* A moved-from wrapper would keep its function table without keeping
     * libibverbs loaded */
    IBVWrapper(const IBVWrapper &)            = delete;
    IBVWrapper &operator=(const IBVWrapper &) = delete;
    IBVWrapper(IBVWrapper &&)                 = delete;
    IBVWrapper &operator=(IBVWrapper &&)      = delete;

    /* True when every required function is available */
    bool is_initialized() const noexcept
    {
        return initialized_;
    }

    struct ibv_device **get_device_list(int *num_devices);
    void                free_device_list(struct ibv_device **list);
    struct ibv_context *open_device(struct ibv_device *device);
    int                 close_device(struct ibv_context *context);
    const char         *get_device_name(struct ibv_device *device);
    int                 query_device(struct ibv_context *context, struct ibv_device_attr *device_attr);
    int            query_port(struct ibv_context *context, uint8_t port_num, struct ibv_port_attr *port_attr);
    int            query_gid(struct ibv_context *context, uint8_t port_num, int index, union ibv_gid *gid);
    struct ibv_pd *alloc_pd(struct ibv_context *context);
    int            dealloc_pd(struct ibv_pd *pd);
    struct ibv_mr *reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access);
    struct ibv_mr *reg_mr_host(struct ibv_pd *pd, void *addr, size_t length, int access);
    int            dereg_mr(struct ibv_mr *mr);
    struct ibv_cq *create_cq(struct ibv_context *context, int cqe, void *cq_context,
                             struct ibv_comp_channel *channel, int comp_vector);
    int            destroy_cq(struct ibv_cq *cq);
    struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr);
    int            modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask);
    int            destroy_qp(struct ibv_qp *qp);
    int            poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc);
    int            post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr);
    int            post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr);

private:
    bool is_dmabuf_supported();
    void init_dmabuf_settings();
    bool init_function_table();

    DlHandle ibv_handle_;
    IbvFuncs funcs_{};
    bool     initialized_    = false;
    bool     dmabuf_enabled_ = true;
    bool     dmabuf_debug_   = false;
};

/* Returns the wrapper that every libibverbs call goes through: the one
 * installed with setIbvForTest(), or otherwise the library's own, which
 * loads libibverbs on the first call */
IBVWrapper &ibv();

/* Installs a wrapper for ibv() to return, and returns the previously
 * installed one (nullptr when the library's own was active). Passing
 * nullptr restores the library's own. Unit tests only; production never
 * calls it. Not synchronized, so call it only while no other thread can
 * be calling ibv(). */
IBVWrapper *setIbvForTest(IBVWrapper *wrapper);

} // namespace hipObj
