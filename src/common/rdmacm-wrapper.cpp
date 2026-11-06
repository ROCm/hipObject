/* SPDX-License-Identifier: MIT
 */

#include "rdmacm-wrapper.h"

#include <cstdint>

#include <sys/socket.h>

#include "ibv-core.h"
#include "rdmacm-core.h"

/* librdmacm's functions, as <rdma/rdma_cma.h> declares them, with
 * rdmacm-core.h's and ibv-core.h's types, which can't be used alongside
 * that header's (see rdmacm-core.h). This file is the only code that calls
 * them. */
extern "C" {
rdma_event_channel *rdma_create_event_channel();
void                rdma_destroy_event_channel(rdma_event_channel *channel);
int         rdma_create_id(rdma_event_channel *channel, rdma_cm_id **id, void *context, rdma_port_space ps);
int         rdma_destroy_id(rdma_cm_id *id);
int         rdma_bind_addr(rdma_cm_id *id, sockaddr *addr);
int         rdma_listen(rdma_cm_id *id, int backlog);
int         rdma_get_cm_event(rdma_event_channel *channel, rdma_cm_event **event);
int         rdma_ack_cm_event(rdma_cm_event *event);
int         rdma_create_qp(rdma_cm_id *id, ibv_pd *pd, ibv_qp_init_attr *qp_init_attr);
void        rdma_destroy_qp(rdma_cm_id *id);
int         rdma_accept(rdma_cm_id *id, rdma_conn_param *conn_param);
int         rdma_reject(rdma_cm_id *id, const void *private_data, uint8_t private_data_len);
int         rdma_disconnect(rdma_cm_id *id);
const char *rdma_event_str(rdma_cm_event_type event);
}

namespace hipObj {

rdma_event_channel *
SystemRdmaCm::create_event_channel() const
{
    return rdma_create_event_channel();
}

void
SystemRdmaCm::destroy_event_channel(rdma_event_channel *channel) const
{
    rdma_destroy_event_channel(channel);
}

int
SystemRdmaCm::create_id(rdma_event_channel *channel, rdma_cm_id **id, void *context, rdma_port_space ps) const
{
    return rdma_create_id(channel, id, context, ps);
}

int
SystemRdmaCm::destroy_id(rdma_cm_id *id) const
{
    return rdma_destroy_id(id);
}

int
SystemRdmaCm::bind_addr(rdma_cm_id *id, sockaddr *addr) const
{
    return rdma_bind_addr(id, addr);
}

int
SystemRdmaCm::listen(rdma_cm_id *id, int backlog) const
{
    return rdma_listen(id, backlog);
}

int
SystemRdmaCm::get_cm_event(rdma_event_channel *channel, rdma_cm_event **event) const
{
    return rdma_get_cm_event(channel, event);
}

int
SystemRdmaCm::ack_cm_event(rdma_cm_event *event) const
{
    return rdma_ack_cm_event(event);
}

int
SystemRdmaCm::create_qp(rdma_cm_id *id, ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) const
{
    return rdma_create_qp(id, pd, qp_init_attr);
}

void
SystemRdmaCm::destroy_qp(rdma_cm_id *id) const
{
    rdma_destroy_qp(id);
}

int
SystemRdmaCm::accept(rdma_cm_id *id, rdma_conn_param *conn_param) const
{
    return rdma_accept(id, conn_param);
}

int
SystemRdmaCm::reject(rdma_cm_id *id, const void *private_data, uint8_t private_data_len) const
{
    return rdma_reject(id, private_data, private_data_len);
}

int
SystemRdmaCm::disconnect(rdma_cm_id *id) const
{
    return rdma_disconnect(id);
}

const char *
SystemRdmaCm::event_str(rdma_cm_event_type event) const
{
    return rdma_event_str(event);
}

} // namespace hipObj
