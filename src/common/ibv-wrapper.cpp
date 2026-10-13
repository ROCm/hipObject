/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * IBV wrapper implementation. Derived from ROCm/rocSHMEM
 * src/gda/ibv_wrapper.cpp via rocm-xio src/common/ibv-wrapper.cpp.
 * Loads libibverbs via dlopen; no link dependency.
 */

#include "ibv-wrapper.h"

#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

#include <dlfcn.h>

#include "hip-seam.h"
#include "hipobj-warnings.h"
#include "ibv-core.h"
#include "ibv-ptr.h"
#include "no-destructor.h"

namespace hipObj {

// Wrapper installed by tests; null means the library's own is active.
// The library itself never swaps the pointer.
static IBVWrapper *g_ibv_override = nullptr;

IBVWrapper &
ibv()
{
    /* Checked first, so a test that installs fakes never loads
     * libibverbs */
    if (g_ibv_override) {
        return *g_ibv_override;
    }

    /* Created on the first call, not when the library loads, so an
     * application that never initializes hipObject never loads
     * libibverbs. Never destroyed, so libibverbs stays loaded until
     * exit: a global holding verbs objects can outlive a wrapper that
     * was created after it. Clang still warns, since NoDestructor's
     * empty destructor isn't trivial. */
    HIPOBJ_WARN_NO_EXIT_DTOR_OFF
    static NoDestructor<IBVWrapper> wrapper;
    HIPOBJ_WARN_NO_EXIT_DTOR_ON
    return *wrapper;
}

IBVWrapper *
setIbvForTest(IBVWrapper *wrapper)
{
    IBVWrapper *previous = g_ibv_override;
    g_ibv_override       = wrapper;
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

    /* Owns a dmabuf fd from HipRuntime::exportDmabuf(), and closes it
     * with the same runtime's closeDmabuf(). Only the registration needs
     * it: ibv_reg_dmabuf_mr() takes its own reference to the dma-buf,
     * which the MR keeps until it's deregistered, so the fd is closed as
     * soon as the registration call returns, whether or not it
     * succeeded. */
    class DmabufFd {
    public:
        DmabufFd(int fd, const HipRuntime &runtime) noexcept : fd_(fd), runtime_(&runtime)
        {
        }
        ~DmabufFd()
        {
            (void)runtime_->closeDmabuf(fd_);
        }

        DmabufFd(const DmabufFd &)            = delete;
        DmabufFd &operator=(const DmabufFd &) = delete;
        DmabufFd(DmabufFd &&)                 = delete;
        DmabufFd &operator=(DmabufFd &&)      = delete;

        int get() const noexcept
        {
            return fd_;
        }

    private:
        int               fd_;
        const HipRuntime *runtime_;
    };

} // anonymous namespace

IBVWrapper::IBVWrapper() : ibv_handle_(open_libibverbs())
{
    init_dmabuf_settings();

    if (!ibv_handle_) {
        fprintf(stderr, "hipObj: Could not open libibverbs.so. RDMA disabled.\n");
        return;
    }

    if (!init_function_table()) {
        fprintf(stderr, "hipObj: Could not init IBV function table. RDMA disabled.\n");
        return;
    }

    initialized_ = true;
}

IBVWrapper::IBVWrapper(const IbvFuncs &funcs) : funcs_(funcs), initialized_(true)
{
    init_dmabuf_settings();
}

void
IBVWrapper::init_dmabuf_settings()
{
    const char *dbg = getenv("HIPOBJ_RDMA_DEBUG");
    dmabuf_debug_   = dbg && *dbg && dbg[0] != '0';

    const char *env = getenv("HIPOBJ_DMABUF");
    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N')) {
        dmabuf_enabled_ = false;
    }
}

bool
IBVWrapper::is_dmabuf_supported()
{
    /* The only gate that can be answered here. Whether a given GPU can export
     * a dmabuf and a given NIC can register it is answered by trying, in
     * reg_mr(): both calls report their own failure and fall through. This
     * used to additionally require CONFIG_DMABUF_MOVE_NOTIFY=y and
     * CONFIG_PCI_P2PDMA=y from /boot/config-$(uname -r), which is a false
     * negative on any kernel that registers dmabuf MRs without move-notify --
     * the mainline builds the emulated-hardware CI lanes run on do exactly
     * that, and lost GPU-direct to a grep. A config file is a prediction; the
     * registration call is the fact. */
    return dmabuf_enabled_ && funcs_.reg_dmabuf_mr != nullptr;
}

bool
IBVWrapper::init_function_table()
{
    void *const handle = ibv_handle_.get();

    dlsym_load_optional(funcs_.reg_dmabuf_mr, handle, "ibv_reg_dmabuf_mr");
    // poll_cq / post_send / post_recv are static inline helpers in verbs.h
    // that delegate to the per-context ops table; depending on the
    // rdma-core build they may not exist as dynamic symbols. Load them
    // optionally and fall back to the context ops entries, which is what
    // the inline functions themselves call.
    dlsym_load_optional(funcs_.poll_cq, handle, "ibv_poll_cq");
    dlsym_load_optional(funcs_.post_send, handle, "ibv_post_send");
    dlsym_load_optional(funcs_.post_recv, handle, "ibv_post_recv");

    // && stops at the first symbol that's missing
    return dlsym_load(funcs_.get_device_list, handle, "ibv_get_device_list") &&
           dlsym_load(funcs_.free_device_list, handle, "ibv_free_device_list") &&
           dlsym_load(funcs_.open_device, handle, "ibv_open_device") &&
           dlsym_load(funcs_.close_device, handle, "ibv_close_device") &&
           dlsym_load(funcs_.get_device_name, handle, "ibv_get_device_name") &&
           dlsym_load(funcs_.query_device, handle, "ibv_query_device") &&
           dlsym_load(funcs_.query_port, handle, "ibv_query_port") &&
           dlsym_load(funcs_.query_gid, handle, "ibv_query_gid") &&
           dlsym_load(funcs_.alloc_pd, handle, "ibv_alloc_pd") &&
           dlsym_load(funcs_.dealloc_pd, handle, "ibv_dealloc_pd") &&
           dlsym_load(funcs_.reg_mr_iova2, handle, "ibv_reg_mr_iova2") &&
           dlsym_load(funcs_.dereg_mr, handle, "ibv_dereg_mr") &&
           dlsym_load(funcs_.create_cq, handle, "ibv_create_cq") &&
           dlsym_load(funcs_.destroy_cq, handle, "ibv_destroy_cq") &&
           dlsym_load(funcs_.create_qp, handle, "ibv_create_qp") &&
           dlsym_load(funcs_.modify_qp, handle, "ibv_modify_qp") &&
           dlsym_load(funcs_.destroy_qp, handle, "ibv_destroy_qp");
}

struct ibv_device **
IBVWrapper::get_device_list(int *num_devices)
{
    return funcs_.get_device_list(num_devices);
}

void
IBVWrapper::free_device_list(struct ibv_device **list)
{
    funcs_.free_device_list(list);
}

struct ibv_context *
IBVWrapper::open_device(struct ibv_device *device)
{
    return funcs_.open_device(device);
}

int
IBVWrapper::close_device(struct ibv_context *context)
{
    return funcs_.close_device(context);
}

const char *
IBVWrapper::get_device_name(struct ibv_device *device)
{
    return funcs_.get_device_name(device);
}

int
IBVWrapper::query_device(struct ibv_context *context, struct ibv_device_attr *device_attr)
{
    return funcs_.query_device(context, device_attr);
}

int
IBVWrapper::query_port(struct ibv_context *context, uint8_t port_num, struct ibv_port_attr *port_attr)
{
    return funcs_.query_port(context, port_num, port_attr);
}

int
IBVWrapper::query_gid(struct ibv_context *context, uint8_t port_num, int index, union ibv_gid *gid)
{
    return funcs_.query_gid(context, port_num, index, gid);
}

struct ibv_pd *
IBVWrapper::alloc_pd(struct ibv_context *context)
{
    return funcs_.alloc_pd(context);
}

int
IBVWrapper::dealloc_pd(struct ibv_pd *pd)
{
    return funcs_.dealloc_pd(pd);
}

struct ibv_mr *
IBVWrapper::reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access)
{
    if (is_dmabuf_supported()) {
        const HipRuntime &runtime = hip();
        uint64_t          offset  = 0;
        int               fd      = -1;

        const int status = runtime.exportDmabuf(addr, length, &fd, &offset);
        if (status != 0) {
            /* Not an error worth a line on stderr by default: this is also the
             * answer for a host pointer, and the host-buffer paths register far
             * more often than the device ones. The plain path below handles it. */
            if (dmabuf_debug_) {
                fprintf(stderr, "hipObj: hsa_amd_portable_export_dmabuf failed: %d\n", status);
            }
        }
        else {
            const DmabufFd dmabuf(fd, runtime);
            struct ibv_mr *mr = funcs_.reg_dmabuf_mr(pd, offset, length, reinterpret_cast<uintptr_t>(addr),
                                                     dmabuf.get(), access);
            if (mr) {
                return mr;
            }
            const std::string reason = std::generic_category().message(errno);
            fprintf(stderr, "hipObj: ibv_reg_dmabuf_mr failed: %s\n", reason.c_str());
        }
        // Fall through to the plain path: it fails too for device memory, but
        // the caller distinguishes "no MR" from "wrong MR" and we must not
        // invent a registration here.
    }

    return reg_mr_host(pd, addr, length, access);
}

struct ibv_mr *
IBVWrapper::reg_mr_host(struct ibv_pd *pd, void *addr, size_t length, int access)
{
    /* What the ibv_reg_mr() macro in verbs.h does with an access value that
     * isn't a compile-time constant. ibv_reg_mr_iova2() with the address as
     * the IOVA is the same registration as the ibv_reg_mr() function, and
     * also accepts the optional access flags. */
    return funcs_.reg_mr_iova2(pd, addr, length, reinterpret_cast<uintptr_t>(addr), access);
}

int
IBVWrapper::dereg_mr(struct ibv_mr *mr)
{
    return funcs_.dereg_mr(mr);
}

struct ibv_cq *
IBVWrapper::create_cq(struct ibv_context *context, int cqe, void *cq_context,
                      struct ibv_comp_channel *channel, int comp_vector)
{
    return funcs_.create_cq(context, cqe, cq_context, channel, comp_vector);
}

int
IBVWrapper::destroy_cq(struct ibv_cq *cq)
{
    return funcs_.destroy_cq(cq);
}

struct ibv_qp *
IBVWrapper::create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr)
{
    return funcs_.create_qp(pd, qp_init_attr);
}

int
IBVWrapper::modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
{
    return funcs_.modify_qp(qp, attr, attr_mask);
}

int
IBVWrapper::destroy_qp(struct ibv_qp *qp)
{
    return funcs_.destroy_qp(qp);
}

int
IBVWrapper::poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
    if (funcs_.poll_cq) {
        return funcs_.poll_cq(cq, num_entries, wc);
    }
    // Inline-function semantics from verbs.h: delegate to the context ops.
    return cq->context->ops.poll_cq(cq, num_entries, wc);
}

int
IBVWrapper::post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr)
{
    if (funcs_.post_send) {
        return funcs_.post_send(qp, wr, bad_wr);
    }
    return qp->context->ops.post_send(qp, wr, bad_wr);
}

int
IBVWrapper::post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad_wr)
{
    if (funcs_.post_recv) {
        return funcs_.post_recv(qp, wr, bad_wr);
    }
    return qp->context->ops.post_recv(qp, wr, bad_wr);
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

int
ibvDestroy(IbvQpPtr &qp)
{
    struct ibv_qp *raw = qp.release();
    if (!raw) {
        return 0;
    }
    int ret = ibv().destroy_qp(raw);
    if (ret != 0) {
        qp.reset(raw); /* still alive: hand it back for a retry */
    }
    return ret;
}

int
ibvDestroy(IbvCqPtr &cq)
{
    struct ibv_cq *raw = cq.release();
    if (!raw) {
        return 0;
    }
    int ret = ibv().destroy_cq(raw);
    if (ret != 0) {
        cq.reset(raw); /* still alive: hand it back for a retry */
    }
    return ret;
}

} // namespace hipObj
