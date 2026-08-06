/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * IBV wrapper implementation. Adapted from rocm-xio.
 * Loads libibverbs via dlopen; no link dependency.
 */

#include "ibv-wrapper.h"

#include <cstdio>
#include <cstring>

#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <dlfcn.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace hipObj {

IBVWrapper ibv;

namespace {

template <typename FuncPtr>
int dlsym_load(FuncPtr& out, void* handle, const char* prefix,
               const char* name) {
  char full_name[256];
  snprintf(full_name, sizeof(full_name), "%s%s", prefix, name);
  out = reinterpret_cast<FuncPtr>(dlsym(handle, full_name));
  if (!out) {
    fprintf(stderr, "hipObj: dlsym failed for %s: %s\n", full_name, dlerror());
    return -1;
  }
  return 0;
}

template <typename FuncPtr>
void dlsym_load_optional(FuncPtr& out, void* handle, const char* prefix,
                         const char* name) {
  char full_name[256];
  snprintf(full_name, sizeof(full_name), "%s%s", prefix, name);
  out = reinterpret_cast<FuncPtr>(dlsym(handle, full_name));
}

} // anonymous namespace

IBVWrapper::IBVWrapper() {
  ibv_handle_ = dlopen("libibverbs.so", RTLD_NOW);
  if (!ibv_handle_)
    ibv_handle_ = dlopen("/usr/lib/x86_64-linux-gnu/libibverbs.so", RTLD_NOW);
  if (!ibv_handle_) {
    fprintf(stderr, "hipObj: Could not open libibverbs.so. RDMA disabled.\n");
    return;
  }

  if (init_function_table() != 0) {
    fprintf(stderr,
            "hipObj: Could not init IBV function table. RDMA disabled.\n");
    return;
  }

  init_dmabuf_support_flag();
  is_initialized = true;
}

IBVWrapper::~IBVWrapper() {
  is_initialized = false;
  if (ibv_handle_) {
    dlclose(ibv_handle_);
  }
}

void IBVWrapper::init_dmabuf_support_flag() {
  if (!dmabuf_enabled_) {
    dmabuf_is_supported_ = 0;
    return;
  }

  if (!funcs_.reg_dmabuf_mr) {
    dmabuf_is_supported_ = 0;
    return;
  }

  const char kernel_opt1[] = "CONFIG_DMABUF_MOVE_NOTIFY=y";
  const char kernel_opt2[] = "CONFIG_PCI_P2PDMA=y";
  int found_opt1 = 0;
  int found_opt2 = 0;
  struct utsname utsname;
  char kernel_conf_file[128];
  char buf[256];

  if (uname(&utsname) == -1) {
    dmabuf_is_supported_ = 0;
    return;
  }

  snprintf(kernel_conf_file, sizeof(kernel_conf_file), "/boot/config-%s",
           utsname.release);
  FILE* fp = fopen(kernel_conf_file, "r");
  if (!fp) {
    dmabuf_is_supported_ = 0;
    return;
  }

  while (fgets(buf, sizeof(buf), fp)) {
    if (strstr(buf, kernel_opt1))
      found_opt1 = 1;
    if (strstr(buf, kernel_opt2))
      found_opt2 = 1;
    if (found_opt1 && found_opt2) {
      dmabuf_is_supported_ = 1;
      fclose(fp);
      return;
    }
  }
  fclose(fp);
  dmabuf_is_supported_ = 0;
}

bool IBVWrapper::is_dmabuf_supported() {
  return dmabuf_is_supported_ != 0;
}

int IBVWrapper::init_function_table() {
#define LOAD_SYM(field, prefix, name)                                          \
  if (dlsym_load(funcs_.field, ibv_handle_, prefix, name) != 0)                \
    return -1;
#define LOAD_SYM_OPT(field, prefix, name)                                      \
  dlsym_load_optional(funcs_.field, ibv_handle_, prefix, name);

  LOAD_SYM(get_device_list, "ibv_", "get_device_list");
  LOAD_SYM(free_device_list, "ibv_", "free_device_list");
  LOAD_SYM(open_device, "ibv_", "open_device");
  LOAD_SYM(close_device, "ibv_", "close_device");
  LOAD_SYM(get_device_name, "ibv_", "get_device_name");
  LOAD_SYM(query_device, "ibv_", "query_device");
  LOAD_SYM(query_port, "ibv_", "query_port");
  LOAD_SYM(query_gid, "ibv_", "query_gid");
  LOAD_SYM(alloc_pd, "ibv_", "alloc_pd");
  LOAD_SYM(dealloc_pd, "ibv_", "dealloc_pd");
  LOAD_SYM(reg_mr, "ibv_", "reg_mr");
  LOAD_SYM_OPT(reg_dmabuf_mr, "ibv_", "reg_dmabuf_mr");
  LOAD_SYM(reg_mr_iova2, "ibv_", "reg_mr_iova2");
  LOAD_SYM(dereg_mr, "ibv_", "dereg_mr");
  LOAD_SYM(create_cq, "ibv_", "create_cq");
  LOAD_SYM(destroy_cq, "ibv_", "destroy_cq");
  LOAD_SYM(create_qp, "ibv_", "create_qp");
  LOAD_SYM(modify_qp, "ibv_", "modify_qp");
  LOAD_SYM(destroy_qp, "ibv_", "destroy_qp");
  LOAD_SYM(poll_cq, "ibv_", "poll_cq");
  LOAD_SYM(post_send, "ibv_", "post_send");
  LOAD_SYM(post_recv, "ibv_", "post_recv");

#undef LOAD_SYM
#undef LOAD_SYM_OPT
  return 0;
}

struct ibv_device** IBVWrapper::get_device_list(int* num_devices) {
  return funcs_.get_device_list(num_devices);
}

void IBVWrapper::free_device_list(struct ibv_device** list) {
  funcs_.free_device_list(list);
}

struct ibv_context* IBVWrapper::open_device(struct ibv_device* device) {
  return funcs_.open_device(device);
}

int IBVWrapper::close_device(struct ibv_context* context) {
  return funcs_.close_device(context);
}

const char* IBVWrapper::get_device_name(struct ibv_device* device) {
  return funcs_.get_device_name(device);
}

int IBVWrapper::query_device(struct ibv_context* context,
                             struct ibv_device_attr* device_attr) {
  return funcs_.query_device(context, device_attr);
}

int IBVWrapper::query_port(struct ibv_context* context, uint8_t port_num,
                           struct ibv_port_attr* port_attr) {
  return funcs_.query_port(context, port_num, port_attr);
}

int IBVWrapper::query_gid(struct ibv_context* context, uint8_t port_num,
                          int index, union ibv_gid* gid) {
  return funcs_.query_gid(context, port_num, index, gid);
}

struct ibv_pd* IBVWrapper::alloc_pd(struct ibv_context* context) {
  return funcs_.alloc_pd(context);
}

int IBVWrapper::dealloc_pd(struct ibv_pd* pd) {
  return funcs_.dealloc_pd(pd);
}

struct ibv_mr* IBVWrapper::reg_mr(struct ibv_pd* pd, void* addr, size_t length,
                                  int access) {
  if (is_dmabuf_supported()) {
    uint64_t offset = 0;
    int fd = 0;

    hsa_status_t status = hsa_amd_portable_export_dmabuf(addr, length, &fd,
                                                         &offset);
    if (status != HSA_STATUS_SUCCESS) {
      fprintf(stderr, "hipObj: hsa_amd_portable_export_dmabuf failed: %d\n",
              status);
      return nullptr;
    }

    struct ibv_mr* mr = funcs_.reg_dmabuf_mr(pd, offset, length,
                                             (uint64_t)(uintptr_t)addr, fd,
                                             access);
    if (mr)
      dmabuf_fd_map_[(uintptr_t)mr] = fd;

    return mr;
  }

  int is_access_const = __builtin_constant_p(
    ((int)(access)&IBV_ACCESS_OPTIONAL_RANGE) == 0);
  if (is_access_const && (access & IBV_ACCESS_OPTIONAL_RANGE) == 0)
    return funcs_.reg_mr(pd, addr, length, (int)access);
  else
    return funcs_.reg_mr_iova2(pd, addr, length, (uintptr_t)addr, access);
}

struct ibv_mr* IBVWrapper::reg_mr_host(struct ibv_pd* pd, void* addr,
                                       size_t length, int access) {
  int is_access_const = __builtin_constant_p(
    ((int)(access)&IBV_ACCESS_OPTIONAL_RANGE) == 0);
  if (is_access_const && (access & IBV_ACCESS_OPTIONAL_RANGE) == 0)
    return funcs_.reg_mr(pd, addr, length, (int)access);
  else
    return funcs_.reg_mr_iova2(pd, addr, length, (uintptr_t)addr, access);
}

int IBVWrapper::dereg_mr(struct ibv_mr* mr) {
  if (is_dmabuf_supported()) {
    auto it = dmabuf_fd_map_.find((uintptr_t)mr);
    if (it != dmabuf_fd_map_.end()) {
      close(it->second);
      dmabuf_fd_map_.erase(it);
    }
  }
  return funcs_.dereg_mr(mr);
}

struct ibv_cq* IBVWrapper::create_cq(struct ibv_context* context, int cqe,
                                     void* cq_context,
                                     struct ibv_comp_channel* channel,
                                     int comp_vector) {
  return funcs_.create_cq(context, cqe, cq_context, channel, comp_vector);
}

int IBVWrapper::destroy_cq(struct ibv_cq* cq) {
  return funcs_.destroy_cq(cq);
}

struct ibv_qp* IBVWrapper::create_qp(struct ibv_pd* pd,
                                     struct ibv_qp_init_attr* qp_init_attr) {
  return funcs_.create_qp(pd, qp_init_attr);
}

int IBVWrapper::modify_qp(struct ibv_qp* qp, struct ibv_qp_attr* attr,
                          int attr_mask) {
  return funcs_.modify_qp(qp, attr, attr_mask);
}

int IBVWrapper::destroy_qp(struct ibv_qp* qp) {
  return funcs_.destroy_qp(qp);
}

int IBVWrapper::poll_cq(struct ibv_cq* cq, int num_entries, struct ibv_wc* wc) {
  return funcs_.poll_cq(cq, num_entries, wc);
}

int IBVWrapper::post_send(struct ibv_qp* qp, struct ibv_send_wr* wr,
                          struct ibv_send_wr** bad_wr) {
  return funcs_.post_send(qp, wr, bad_wr);
}

int IBVWrapper::post_recv(struct ibv_qp* qp, struct ibv_recv_wr* wr,
                          struct ibv_recv_wr** bad_wr) {
  return funcs_.post_recv(qp, wr, bad_wr);
}

} // namespace hipObj
