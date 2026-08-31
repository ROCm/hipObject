/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

#include "ibv-core.h"

namespace hipObj {

/* ibverbs function table. Public so the unit-test accessor can name
 * the type; production code goes through the wrapper methods and
 * never touches it directly. */
struct IbvFuncs {
  struct ibv_device** (*get_device_list)(int*);
  void (*free_device_list)(struct ibv_device**);
  struct ibv_context* (*open_device)(struct ibv_device*);
  int (*close_device)(struct ibv_context*);
  const char* (*get_device_name)(struct ibv_device*);
  int (*query_device)(struct ibv_context*, struct ibv_device_attr*);
  int (*query_port)(struct ibv_context*, uint8_t, struct ibv_port_attr*);
  int (*query_gid)(struct ibv_context*, uint8_t, int, union ibv_gid*);
  struct ibv_pd* (*alloc_pd)(struct ibv_context*);
  int (*dealloc_pd)(struct ibv_pd*);
  struct ibv_mr* (*reg_mr)(struct ibv_pd*, void*, size_t, int);
  struct ibv_mr* (*reg_dmabuf_mr)(struct ibv_pd*, uint64_t, size_t, uint64_t,
                                  int, int);
  struct ibv_mr* (*reg_mr_iova2)(struct ibv_pd*, void*, size_t, uintptr_t, int);
  int (*dereg_mr)(struct ibv_mr*);
  struct ibv_cq* (*create_cq)(struct ibv_context*, int, void*,
                              struct ibv_comp_channel*, int);
  int (*destroy_cq)(struct ibv_cq*);
  struct ibv_qp* (*create_qp)(struct ibv_pd*, struct ibv_qp_init_attr*);
  int (*modify_qp)(struct ibv_qp*, struct ibv_qp_attr*, int);
  int (*destroy_qp)(struct ibv_qp*);
  int (*poll_cq)(struct ibv_cq*, int, struct ibv_wc*);
  int (*post_send)(struct ibv_qp*, struct ibv_send_wr*, struct ibv_send_wr**);
  int (*post_recv)(struct ibv_qp*, struct ibv_recv_wr*, struct ibv_recv_wr**);
};

class IBVWrapper {
public:
  IBVWrapper();
  ~IBVWrapper();

  bool is_initialized = false;

  struct ibv_device** get_device_list(int* num_devices);
  void free_device_list(struct ibv_device** list);
  struct ibv_context* open_device(struct ibv_device* device);
  int close_device(struct ibv_context* context);
  const char* get_device_name(struct ibv_device* device);
  int query_device(struct ibv_context* context,
                   struct ibv_device_attr* device_attr);
  int query_port(struct ibv_context* context, uint8_t port_num,
                 struct ibv_port_attr* port_attr);
  int query_gid(struct ibv_context* context, uint8_t port_num, int index,
                union ibv_gid* gid);
  struct ibv_pd* alloc_pd(struct ibv_context* context);
  int dealloc_pd(struct ibv_pd* pd);
  struct ibv_mr* reg_mr(struct ibv_pd* pd, void* addr, size_t length,
                        int access);
  struct ibv_mr* reg_mr_host(struct ibv_pd* pd, void* addr, size_t length,
                             int access);
  int dereg_mr(struct ibv_mr* mr);
  struct ibv_cq* create_cq(struct ibv_context* context, int cqe,
                           void* cq_context, struct ibv_comp_channel* channel,
                           int comp_vector);
  int destroy_cq(struct ibv_cq* cq);
  struct ibv_qp* create_qp(struct ibv_pd* pd,
                           struct ibv_qp_init_attr* qp_init_attr);
  int modify_qp(struct ibv_qp* qp, struct ibv_qp_attr* attr, int attr_mask);
  int destroy_qp(struct ibv_qp* qp);
  int poll_cq(struct ibv_cq* cq, int num_entries, struct ibv_wc* wc);
  int post_send(struct ibv_qp* qp, struct ibv_send_wr* wr,
                struct ibv_send_wr** bad_wr);
  int post_recv(struct ibv_qp* qp, struct ibv_recv_wr* wr,
                struct ibv_recv_wr** bad_wr);

private:
  bool is_dmabuf_supported();
  void init_dmabuf_support_flag();
  int init_function_table();

  void* ibv_handle_ = nullptr;
  IbvFuncs funcs_ = {};
  int dmabuf_enabled_ = 1;
  int dmabuf_is_supported_ = 0;
  std::map<uintptr_t, int> dmabuf_fd_map_;

#ifdef HIPOBJ_UNIT_TESTS
public:
  /* Test accessor for the internal function table. Compiled out of
   * the shipped library because the macro is only set for the
   * unit-test object build (see test/unit/CMakeLists.txt). */
  IbvFuncs& funcsForTest() {
    return funcs_;
  }

private:
#endif
};

extern IBVWrapper ibv;

} // namespace hipObj
