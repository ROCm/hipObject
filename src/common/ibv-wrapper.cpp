/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * libibverbs seam implementation. Derived from ROCm/rocSHMEM
 * src/gda/ibv_wrapper.cpp via rocm-xio src/common/ibv-wrapper.cpp.
 * Loads libibverbs via dlopen; no link dependency.
 */

#include "ibv-wrapper.h"

#include <bit>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <dlfcn.h>

#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-ptr.h"
#include "no-destructor.h"

namespace hipObj {

// Verbs installed by tests; null means the library's own are active.
// The library itself never swaps the pointer.
static const Verbs *g_ibv_override = nullptr;

namespace {

    /* The library's own verbs, or null when libibverbs couldn't be
     * loaded. Loaded on the first call, not when the library loads, so
     * an application that never initializes hipObject never loads
     * libibverbs. Never destroyed, so libibverbs stays loaded until exit:
     * a global holding verbs objects can outlive verbs that were loaded
     * after it. Clang still warns, since NoDestructor's empty destructor
     * isn't trivial. */
    const Verbs *libraryVerbs()
    {
        HIPOBJ_WARN_NO_EXIT_DTOR_OFF
        static NoDestructor<std::unique_ptr<LoadedVerbs>> verbs(loadLibibverbs());
        HIPOBJ_WARN_NO_EXIT_DTOR_ON
        return verbs->get();
    }

} // anonymous namespace

const Verbs &
ibv()
{
    /* Checked first, so a test that installs fakes never loads
     * libibverbs */
    if (g_ibv_override) {
        return *g_ibv_override;
    }
    return *libraryVerbs();
}

bool
ibvAvailable()
{
    return g_ibv_override || libraryVerbs();
}

const Verbs *
setIbvForTest(const Verbs *verbs)
{
    const Verbs *previous = g_ibv_override;
    g_ibv_override        = verbs;
    return previous;
}

namespace {

    /* Sets out to the symbol, or to nullptr when the library doesn't have it.
     * Casting dlsym()'s void * to a function pointer is only conditionally
     * supported in C++, so copy the pointer's bits instead (std::bit_cast
     * doesn't compile if the two pointers differ in size). */
    template <typename FuncPtr> void dlsym_load_optional(FuncPtr &out, void *handle, const char *name)
    {
        out = std::bit_cast<FuncPtr>(dlsym(handle, name));
    }

    /* As dlsym_load_optional(), but a missing symbol is an error: it's
     * reported, and the return value is false */
    template <typename FuncPtr> bool dlsym_load(FuncPtr &out, void *handle, const char *name)
    {
        dlsym_load_optional(out, handle, name);
        if (!out) {
            /* dlerror() returns NULL when the symbol's value is NULL */
            const char       *err      = dlerror();
            const std::string symbol   = name;
            const std::string errorStr = err ? err : "symbol is NULL";
            fprintf(stderr, "hipObj: dlsym failed for %s: %s\n", symbol.c_str(), errorStr.c_str());
            return false;
        }
        return true;
    }

    /* Loads libibverbs, trying Debian and Ubuntu's x86-64 library
     * directory when the loader's search path doesn't find it. Empty when
     * neither loads. */
    DlHandle open_libibverbs()
    {
        DlHandle handle(dlopen("libibverbs.so", RTLD_NOW));
        if (!handle) {
            handle.reset(dlopen("/usr/lib/x86_64-linux-gnu/libibverbs.so", RTLD_NOW));
        }
        return handle;
    }

    /* Fills in funcs from libibverbs. False when a function that has to be
     * there is missing. */
    bool load_functions(LoadedVerbs::Funcs &funcs, void *handle)
    {
        dlsym_load_optional(funcs.reg_dmabuf_mr, handle, "ibv_reg_dmabuf_mr");
        // poll_cq / post_send are static inline helpers in verbs.h
        // that delegate to the per-context ops table; depending on the
        // rdma-core build they may not exist as dynamic symbols. Load them
        // optionally and fall back to the context ops entries, which is what
        // the inline functions themselves call.
        dlsym_load_optional(funcs.poll_cq, handle, "ibv_poll_cq");
        dlsym_load_optional(funcs.post_send, handle, "ibv_post_send");

        // && stops at the first symbol that's missing
        return dlsym_load(funcs.get_device_list, handle, "ibv_get_device_list") &&
               dlsym_load(funcs.free_device_list, handle, "ibv_free_device_list") &&
               dlsym_load(funcs.open_device, handle, "ibv_open_device") &&
               dlsym_load(funcs.close_device, handle, "ibv_close_device") &&
               dlsym_load(funcs.get_device_name, handle, "ibv_get_device_name") &&
               dlsym_load(funcs.query_device, handle, "ibv_query_device") &&
               dlsym_load(funcs.query_port, handle, "ibv_query_port") &&
               dlsym_load(funcs.query_gid, handle, "ibv_query_gid") &&
               dlsym_load(funcs.alloc_pd, handle, "ibv_alloc_pd") &&
               dlsym_load(funcs.dealloc_pd, handle, "ibv_dealloc_pd") &&
               dlsym_load(funcs.reg_mr_iova2, handle, "ibv_reg_mr_iova2") &&
               dlsym_load(funcs.dereg_mr, handle, "ibv_dereg_mr") &&
               dlsym_load(funcs.create_cq, handle, "ibv_create_cq") &&
               dlsym_load(funcs.destroy_cq, handle, "ibv_destroy_cq") &&
               dlsym_load(funcs.create_qp, handle, "ibv_create_qp") &&
               dlsym_load(funcs.modify_qp, handle, "ibv_modify_qp") &&
               dlsym_load(funcs.destroy_qp, handle, "ibv_destroy_qp");
    }

} // anonymous namespace

std::unique_ptr<LoadedVerbs>
loadLibibverbs()
{
    DlHandle library = open_libibverbs();
    if (!library) {
        fprintf(stderr, "hipObj: Could not open libibverbs.so. RDMA disabled.\n");
        return nullptr;
    }

    LoadedVerbs::Funcs funcs{};
    if (!load_functions(funcs, library.get())) {
        fprintf(stderr, "hipObj: Could not init IBV function table. RDMA disabled.\n");
        return nullptr;
    }

    return std::make_unique<LoadedVerbs>(funcs, std::move(library));
}

LoadedVerbs::LoadedVerbs(const Funcs &funcs, DlHandle library) : library_(std::move(library)), funcs_(funcs)
{
}

struct ibv_device **
LoadedVerbs::get_device_list(int *num_devices) const
{
    return funcs_.get_device_list(num_devices);
}

void
LoadedVerbs::free_device_list(struct ibv_device **list) const
{
    funcs_.free_device_list(list);
}

struct ibv_context *
LoadedVerbs::open_device(struct ibv_device *device) const
{
    return funcs_.open_device(device);
}

int
LoadedVerbs::close_device(struct ibv_context *context) const
{
    return funcs_.close_device(context);
}

const char *
LoadedVerbs::get_device_name(struct ibv_device *device) const
{
    return funcs_.get_device_name(device);
}

int
LoadedVerbs::query_device(struct ibv_context *context, struct ibv_device_attr *device_attr) const
{
    return funcs_.query_device(context, device_attr);
}

int
LoadedVerbs::query_port(struct ibv_context *context, uint8_t port_num, struct ibv_port_attr *port_attr) const
{
    return funcs_.query_port(context, port_num, port_attr);
}

int
LoadedVerbs::query_gid(struct ibv_context *context, uint8_t port_num, int index, union ibv_gid *gid) const
{
    return funcs_.query_gid(context, port_num, index, gid);
}

struct ibv_pd *
LoadedVerbs::alloc_pd(struct ibv_context *context) const
{
    return funcs_.alloc_pd(context);
}

int
LoadedVerbs::dealloc_pd(struct ibv_pd *pd) const
{
    return funcs_.dealloc_pd(pd);
}

struct ibv_mr *
LoadedVerbs::reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access) const
{
    /* What the ibv_reg_mr() macro in verbs.h does with an access value that
     * isn't a compile-time constant. ibv_reg_mr_iova2() with the address as
     * the IOVA is the same registration as the ibv_reg_mr() function, and
     * also accepts the optional access flags. */
    return funcs_.reg_mr_iova2(pd, addr, length, reinterpret_cast<uintptr_t>(addr), access);
}

bool
LoadedVerbs::supportsDmabuf() const
{
    return funcs_.reg_dmabuf_mr != nullptr;
}

struct ibv_mr *
LoadedVerbs::reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd,
                           int access) const
{
    if (!funcs_.reg_dmabuf_mr) {
        errno = EOPNOTSUPP;
        return nullptr;
    }
    return funcs_.reg_dmabuf_mr(pd, offset, length, iova, fd, access);
}

int
LoadedVerbs::dereg_mr(struct ibv_mr *mr) const
{
    return funcs_.dereg_mr(mr);
}

struct ibv_cq *
LoadedVerbs::create_cq(struct ibv_context *context, int cqe, void *cq_context,
                       struct ibv_comp_channel *channel, int comp_vector) const
{
    return funcs_.create_cq(context, cqe, cq_context, channel, comp_vector);
}

int
LoadedVerbs::destroy_cq(struct ibv_cq *cq) const
{
    return funcs_.destroy_cq(cq);
}

struct ibv_qp *
LoadedVerbs::create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) const
{
    return funcs_.create_qp(pd, qp_init_attr);
}

int
LoadedVerbs::modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask) const
{
    return funcs_.modify_qp(qp, attr, attr_mask);
}

int
LoadedVerbs::destroy_qp(struct ibv_qp *qp) const
{
    return funcs_.destroy_qp(qp);
}

int
LoadedVerbs::poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc) const
{
    if (funcs_.poll_cq) {
        return funcs_.poll_cq(cq, num_entries, wc);
    }
    // Inline-function semantics from verbs.h: delegate to the context ops.
    return cq->context->ops.poll_cq(cq, num_entries, wc);
}

int
LoadedVerbs::post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr) const
{
    if (funcs_.post_send) {
        return funcs_.post_send(qp, wr, bad_wr);
    }
    return qp->context->ops.post_send(qp, wr, bad_wr);
}

// ---- Smart pointer deleters (ibv-ptr.h) -----------------------------

void
IbvContextDeleter::operator()(struct ibv_context *ctx) const noexcept
{
    (void)ibv().close_device(ctx);
}

void
IbvPdDeleter::operator()(struct ibv_pd *pd) const noexcept
{
    (void)ibv().dealloc_pd(pd);
}

void
IbvCqDeleter::operator()(struct ibv_cq *cq) const noexcept
{
    (void)ibv().destroy_cq(cq);
}

void
IbvQpDeleter::operator()(struct ibv_qp *qp) const noexcept
{
    (void)ibv().destroy_qp(qp);
}

void
IbvMrDeleter::operator()(struct ibv_mr *mr) const noexcept
{
    (void)ibv().dereg_mr(mr);
}

void
IbvDeviceListDeleter::operator()(struct ibv_device **list) const noexcept
{
    ibv().free_device_list(list);
}

} // namespace hipObj
