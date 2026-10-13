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

    virtual struct ibv_device **get_device_list(int *num_devices) const                              = 0;
    virtual void                free_device_list(struct ibv_device **list) const                     = 0;
    virtual struct ibv_context *open_device(struct ibv_device *device) const                         = 0;
    virtual int                 close_device(struct ibv_context *context) const                      = 0;
    virtual const char         *get_device_name(struct ibv_device *device) const                     = 0;
    virtual int query_device(struct ibv_context *context, struct ibv_device_attr *device_attr) const = 0;
    virtual int query_port(struct ibv_context *context, uint8_t port_num,
                           struct ibv_port_attr *port_attr) const                                    = 0;
    virtual int query_gid(struct ibv_context *context, uint8_t port_num, int index,
                          union ibv_gid *gid) const                                                  = 0;
    virtual struct ibv_pd *alloc_pd(struct ibv_context *context) const                               = 0;
    virtual int            dealloc_pd(struct ibv_pd *pd) const                                       = 0;

    /* Registers memory the NIC reaches through the CPU's page tables, with
     * its address as the IOVA */
    virtual struct ibv_mr *reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access) const = 0;

    /* True when libibverbs has ibv_reg_dmabuf_mr(), which rdma-core 34
     * added. Without it, reg_dmabuf_mr() fails with EOPNOTSUPP. */
    virtual bool           supportsDmabuf() const                                                       = 0;
    virtual struct ibv_mr *reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova,
                                         int fd, int access) const                                      = 0;
    virtual int            dereg_mr(struct ibv_mr *mr) const                                            = 0;
    virtual struct ibv_cq *create_cq(struct ibv_context *context, int cqe, void *cq_context,
                                     struct ibv_comp_channel *channel, int comp_vector) const           = 0;
    virtual int            destroy_cq(struct ibv_cq *cq) const                                          = 0;
    virtual struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) const    = 0;
    virtual int            modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask) const  = 0;
    virtual int            destroy_qp(struct ibv_qp *qp) const                                          = 0;
    virtual int            poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc) const         = 0;
    virtual int post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr) const = 0;
    virtual int post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr) const = 0;
};

/* libibverbs, through the functions loadLibibverbs() loads from it */
class LoadedVerbs final : public Verbs {
public:
    /* The libibverbs functions it calls */
    struct Funcs {
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
    };

    /* Calls funcs, which came from library, and keeps library loaded for
     * as long as it lives. Every entry but reg_dmabuf_mr, poll_cq,
     * post_send, and post_recv has to be set. Without poll_cq, post_send,
     * or post_recv, it calls the provider's entry in the context's ops,
     * as the inline functions in <infiniband/verbs.h> do. */
    explicit LoadedVerbs(const Funcs &funcs, DlHandle library = DlHandle{});

    struct ibv_device **get_device_list(int *num_devices) const override;
    void                free_device_list(struct ibv_device **list) const override;
    struct ibv_context *open_device(struct ibv_device *device) const override;
    int                 close_device(struct ibv_context *context) const override;
    const char         *get_device_name(struct ibv_device *device) const override;
    int query_device(struct ibv_context *context, struct ibv_device_attr *device_attr) const override;
    int query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr) const override;
    int query_gid(struct ibv_context *context, uint8_t port_num, int index,
                  union ibv_gid *gid) const override;
    struct ibv_pd *alloc_pd(struct ibv_context *context) const override;
    int            dealloc_pd(struct ibv_pd *pd) const override;
    struct ibv_mr *reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access) const override;
    bool           supportsDmabuf() const override;
    struct ibv_mr *reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd,
                                 int access) const override;
    int            dereg_mr(struct ibv_mr *mr) const override;
    struct ibv_cq *create_cq(struct ibv_context *context, int cqe, void *cq_context,
                             struct ibv_comp_channel *channel, int comp_vector) const override;
    int            destroy_cq(struct ibv_cq *cq) const override;
    struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) const override;
    int            modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask) const override;
    int            destroy_qp(struct ibv_qp *qp) const override;
    int            poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc) const override;
    int post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr) const override;
    int post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr) const override;

private:
    DlHandle library_;
    Funcs    funcs_;
};

/* Loads libibverbs and the functions LoadedVerbs calls. Null, after a
 * message on stderr, when libibverbs can't be loaded or lacks a function
 * that has to be there. */
std::unique_ptr<LoadedVerbs> loadLibibverbs();

/* Returns the verbs every libibverbs call goes through: the ones
 * installed with setIbvForTest(), or otherwise the library's own, which
 * load libibverbs on the first call. Call it only once ibvAvailable() has
 * returned true. */
const Verbs &ibv();

/* Whether ibv() has verbs to return: true when a test installed some,
 * or when libibverbs loads. Loads it, unless a test installed verbs. */
bool ibvAvailable();

/* Installs verbs for ibv() to return, and returns the previously
 * installed ones (nullptr when the library's own were active). Passing
 * nullptr restores the library's own. Unit tests only; production never
 * calls it. Not synchronized, so call it only while no other thread can
 * be calling ibv(). */
const Verbs *setIbvForTest(const Verbs *verbs);

} // namespace hipObj
