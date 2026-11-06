/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* libibverbs seam.
 *
 * The library calls libibverbs through Verbs, so unit tests can
 * substitute a fake (no RDMA device required). LoadedVerbs, the
 * production implementation, calls the functions it loads from
 * libibverbs with dlopen(), so the library doesn't link to it. Each
 * method does what the libibverbs function it's named after does, and
 * nothing more: deciding how to register memory is the caller's job.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "dl-handle.h"
#include "ibv-core.h"

namespace hipObj {

/* The methods are const, so one Verbs can be shared by everything that
 * uses it. A fake that records its calls keeps what it records outside
 * the object, behind a pointer. */
class Verbs {
public:
    Verbs()          = default;
    virtual ~Verbs() = default;

    /* Copying through a reference to the base would slice */
    Verbs(const Verbs &)            = delete;
    Verbs &operator=(const Verbs &) = delete;
    Verbs(Verbs &&)                 = delete;
    Verbs &operator=(Verbs &&)      = delete;

    [[nodiscard]] virtual ibv_device **get_device_list(int *num_devices) const                           = 0;
    virtual void                       free_device_list(ibv_device **list) const                         = 0;
    [[nodiscard]] virtual ibv_context *open_device(ibv_device *device) const                             = 0;
    [[nodiscard]] virtual int          close_device(ibv_context *context) const                          = 0;
    [[nodiscard]] virtual const char  *get_device_name(ibv_device *device) const                         = 0;
    [[nodiscard]] virtual int     query_device(ibv_context *context, ibv_device_attr *device_attr) const = 0;
    [[nodiscard]] virtual int     query_port(ibv_context *context, uint8_t port_num,
                                             ibv_port_attr *port_attr) const                             = 0;
    [[nodiscard]] virtual int     query_gid(ibv_context *context, uint8_t port_num, int index,
                                            ibv_gid *gid) const                                          = 0;
    [[nodiscard]] virtual ibv_pd *alloc_pd(ibv_context *context) const                                   = 0;
    [[nodiscard]] virtual int     dealloc_pd(ibv_pd *pd) const                                           = 0;

    /* Registers memory the NIC reaches through the CPU's page tables, with
     * its address as the IOVA */
    [[nodiscard]] virtual ibv_mr *reg_mr(ibv_pd *pd, void *addr, size_t length, int access) const = 0;

    /* True when libibverbs has ibv_reg_dmabuf_mr(), which rdma-core 34
     * added. Without it, reg_dmabuf_mr() fails with EOPNOTSUPP. */
    [[nodiscard]] virtual bool    supportsDmabuf() const                                             = 0;
    [[nodiscard]] virtual ibv_mr *reg_dmabuf_mr(ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova,
                                                int fd, int access) const                            = 0;
    [[nodiscard]] virtual int     dereg_mr(ibv_mr *mr) const                                         = 0;
    [[nodiscard]] virtual ibv_cq *create_cq(ibv_context *context, int cqe, void *cq_context,
                                            ibv_comp_channel *channel, int comp_vector) const        = 0;
    [[nodiscard]] virtual int     destroy_cq(ibv_cq *cq) const                                       = 0;
    [[nodiscard]] virtual ibv_qp *create_qp(ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const        = 0;
    [[nodiscard]] virtual int     modify_qp(ibv_qp *qp, ibv_qp_attr *attr, int attr_mask) const      = 0;
    [[nodiscard]] virtual int     destroy_qp(ibv_qp *qp) const                                       = 0;
    [[nodiscard]] virtual int     poll_cq(ibv_cq *cq, int num_entries, ibv_wc *wc) const             = 0;
    [[nodiscard]] virtual int     post_send(ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) const = 0;

    /* The context's async events, which the RC-CM listener (v3) logs */
    [[nodiscard]] virtual int         get_async_event(ibv_context *context, ibv_async_event *event) const = 0;
    virtual void                      ack_async_event(ibv_async_event *event) const                       = 0;
    [[nodiscard]] virtual const char *event_type_str(ibv_event_type event) const                          = 0;
};

/* libibverbs, through the functions loadLibibverbs() loads from it */
class LoadedVerbs final : public Verbs {
public:
    /* The libibverbs functions it calls */
    struct Funcs {
        ibv_device **(*get_device_list)(int *);
        void (*free_device_list)(ibv_device **);
        ibv_context *(*open_device)(ibv_device *);
        int (*close_device)(ibv_context *);
        const char *(*get_device_name)(ibv_device *);
        int (*query_device)(ibv_context *, ibv_device_attr *);
        int (*query_port)(ibv_context *, uint8_t, ibv_port_attr *);
        int (*query_gid)(ibv_context *, uint8_t, int, ibv_gid *);
        ibv_pd *(*alloc_pd)(ibv_context *);
        int (*dealloc_pd)(ibv_pd *);
        ibv_mr *(*reg_dmabuf_mr)(ibv_pd *, uint64_t, size_t, uint64_t, int, int);
        ibv_mr *(*reg_mr_iova2)(ibv_pd *, void *, size_t, uintptr_t, int);
        int (*dereg_mr)(ibv_mr *);
        ibv_cq *(*create_cq)(ibv_context *, int, void *, ibv_comp_channel *, int);
        int (*destroy_cq)(ibv_cq *);
        ibv_qp *(*create_qp)(ibv_pd *, ibv_qp_init_attr *);
        int (*modify_qp)(ibv_qp *, ibv_qp_attr *, int);
        int (*destroy_qp)(ibv_qp *);
        int (*poll_cq)(ibv_cq *, int, ibv_wc *);
        int (*post_send)(ibv_qp *, ibv_send_wr *, ibv_send_wr **);
        int (*get_async_event)(ibv_context *, ibv_async_event *);
        void (*ack_async_event)(ibv_async_event *);
        const char *(*event_type_str)(ibv_event_type);
    };

    /* Calls funcs, which came from library, and keeps library loaded for
     * as long as it lives. Every entry but reg_dmabuf_mr, poll_cq, and
     * post_send has to be set. Without poll_cq or post_send, it calls the
     * provider's entry in the context's ops, as the inline functions in
     * <infiniband/verbs.h> do. */
    explicit LoadedVerbs(const Funcs &funcs, DlHandle library = DlHandle{});

    [[nodiscard]] ibv_device **get_device_list(int *num_devices) const override;
    void                       free_device_list(ibv_device **list) const override;
    [[nodiscard]] ibv_context *open_device(ibv_device *device) const override;
    [[nodiscard]] int          close_device(ibv_context *context) const override;
    [[nodiscard]] const char  *get_device_name(ibv_device *device) const override;
    [[nodiscard]] int     query_device(ibv_context *context, ibv_device_attr *device_attr) const override;
    [[nodiscard]] int     query_port(ibv_context *context, uint8_t port_num,
                                     ibv_port_attr *port_attr) const override;
    [[nodiscard]] int     query_gid(ibv_context *context, uint8_t port_num, int index,
                                    ibv_gid *gid) const override;
    [[nodiscard]] ibv_pd *alloc_pd(ibv_context *context) const override;
    [[nodiscard]] int     dealloc_pd(ibv_pd *pd) const override;
    [[nodiscard]] ibv_mr *reg_mr(ibv_pd *pd, void *addr, size_t length, int access) const override;
    [[nodiscard]] bool    supportsDmabuf() const override;
    [[nodiscard]] ibv_mr *reg_dmabuf_mr(ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd,
                                        int access) const override;
    [[nodiscard]] int     dereg_mr(ibv_mr *mr) const override;
    [[nodiscard]] ibv_cq *create_cq(ibv_context *context, int cqe, void *cq_context,
                                    ibv_comp_channel *channel, int comp_vector) const override;
    [[nodiscard]] int     destroy_cq(ibv_cq *cq) const override;
    [[nodiscard]] ibv_qp *create_qp(ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const override;
    [[nodiscard]] int     modify_qp(ibv_qp *qp, ibv_qp_attr *attr, int attr_mask) const override;
    [[nodiscard]] int     destroy_qp(ibv_qp *qp) const override;
    [[nodiscard]] int     poll_cq(ibv_cq *cq, int num_entries, ibv_wc *wc) const override;
    [[nodiscard]] int     post_send(ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) const override;

    int                       get_async_event(ibv_context *context, ibv_async_event *event) const override;
    void                      ack_async_event(ibv_async_event *event) const override;
    [[nodiscard]] const char *event_type_str(ibv_event_type event) const override;

private:
    DlHandle library_;
    Funcs    funcs_;
};

/* Loads libibverbs and the functions LoadedVerbs calls. Null, after a
 * message on stderr, when libibverbs can't be loaded or lacks a function
 * that has to be there. */
std::unique_ptr<LoadedVerbs> loadLibibverbs();

} // namespace hipObj
