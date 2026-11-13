/* SPDX-License-Identifier: MIT
 */

/* librdmacm seam.
 *
 * The RC-CM listener (rc-listener.h) calls librdmacm through RdmaCm, so
 * unit tests can substitute a fake (no RDMA device required).
 * SystemRdmaCm, the production implementation, calls the librdmacm the
 * library links to in a v3 build (common/rdmacm-wrapper.cpp, which a v1
 * build doesn't compile). Each method does what the librdmacm function it's
 * named after does, and nothing more.
 */

#pragma once

#include <cstdint>

#include "ibv-core.h"
#include "rdmacm-core.h"

struct sockaddr;

namespace hipObj {

/* The methods are const, as Verbs's are, so one RdmaCm can be shared by
 * everything that uses it */
class RdmaCm {
public:
    RdmaCm()          = default;
    virtual ~RdmaCm() = default;

    /* Copying through a reference to the base would slice */
    RdmaCm(const RdmaCm &)            = delete;
    RdmaCm &operator=(const RdmaCm &) = delete;
    RdmaCm(RdmaCm &&)                 = delete;
    RdmaCm &operator=(RdmaCm &&)      = delete;

    [[nodiscard]] virtual rdma_event_channel *create_event_channel() const                                = 0;
    virtual void                              destroy_event_channel(rdma_event_channel *channel) const    = 0;
    [[nodiscard]] virtual int create_id(rdma_event_channel *channel, rdma_cm_id **id, void *context,
                                        rdma_port_space ps) const                                         = 0;
    [[nodiscard]] virtual int destroy_id(rdma_cm_id *id) const                                            = 0;
    [[nodiscard]] virtual int bind_addr(rdma_cm_id *id, sockaddr *addr) const                             = 0;
    [[nodiscard]] virtual int listen(rdma_cm_id *id, int backlog) const                                   = 0;
    [[nodiscard]] virtual int get_cm_event(rdma_event_channel *channel, rdma_cm_event **event) const      = 0;
    [[nodiscard]] virtual int ack_cm_event(rdma_cm_event *event) const                                    = 0;
    [[nodiscard]] virtual int create_qp(rdma_cm_id *id, ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const = 0;
    virtual void              destroy_qp(rdma_cm_id *id) const                                            = 0;
    [[nodiscard]] virtual int accept(rdma_cm_id *id, rdma_conn_param *conn_param) const                   = 0;
    [[nodiscard]] virtual int reject(rdma_cm_id *id, const void *private_data,
                                     uint8_t private_data_len) const                                      = 0;
    [[nodiscard]] virtual int disconnect(rdma_cm_id *id) const                                            = 0;
    [[nodiscard]] virtual const char *event_str(rdma_cm_event_type event) const                           = 0;
};

/* librdmacm, which the library links to */
class SystemRdmaCm final : public RdmaCm {
public:
    SystemRdmaCm()           = default;
    ~SystemRdmaCm() override = default;

    SystemRdmaCm(const SystemRdmaCm &)            = delete;
    SystemRdmaCm &operator=(const SystemRdmaCm &) = delete;
    SystemRdmaCm(SystemRdmaCm &&)                 = delete;
    SystemRdmaCm &operator=(SystemRdmaCm &&)      = delete;

    [[nodiscard]] rdma_event_channel *create_event_channel() const override;
    void                              destroy_event_channel(rdma_event_channel *channel) const override;
    [[nodiscard]] int                 create_id(rdma_event_channel *channel, rdma_cm_id **id, void *context,
                                                rdma_port_space ps) const override;
    [[nodiscard]] int                 destroy_id(rdma_cm_id *id) const override;
    [[nodiscard]] int                 bind_addr(rdma_cm_id *id, sockaddr *addr) const override;
    [[nodiscard]] int                 listen(rdma_cm_id *id, int backlog) const override;
    [[nodiscard]] int get_cm_event(rdma_event_channel *channel, rdma_cm_event **event) const override;
    [[nodiscard]] int ack_cm_event(rdma_cm_event *event) const override;
    [[nodiscard]] int create_qp(rdma_cm_id *id, ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const override;
    void              destroy_qp(rdma_cm_id *id) const override;
    [[nodiscard]] int accept(rdma_cm_id *id, rdma_conn_param *conn_param) const override;
    [[nodiscard]] int reject(rdma_cm_id *id, const void *private_data,
                             uint8_t private_data_len) const override;
    [[nodiscard]] int disconnect(rdma_cm_id *id) const override;
    [[nodiscard]] const char *event_str(rdma_cm_event_type event) const override;
};

} // namespace hipObj
