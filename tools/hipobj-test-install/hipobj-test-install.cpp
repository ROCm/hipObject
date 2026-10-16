/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* hipobj-test-install: checks that hipObject is installed and wired up
 * correctly, as far as the system's hardware allows
 *
 * It needs no S3 server, and checks what it can without a GPU or an RDMA
 * NIC, reporting what it can't check as skipped, not failed. It never
 * moves data: that needs a server to do the RDMA write.
 *
 * So far it only reports the hipObject version. The comments in main()
 * describe the checks it will make.
 */

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include <hipobj.h>

#include "hipobj-span.h"

namespace {

void
usage(FILE *stream, const std::string &prog)
{
    fprintf(stream, "usage: %s [--version] [--help]\n", prog.c_str());
}

} // namespace

/* Each check will print one line: PASS, FAIL, or SKIP, the step, and a
 * detail. The checks stop at the first failure that later steps depend
 * on. Missing hardware is a SKIP, and the steps that need it are skipped
 * too.
 *
 * Exit status: 0 if every check that ran passed (skips included, unless
 * --require makes them failures), 1 if one failed, 2 for a usage error.
 */
int
main(int argc, char *argv[])
{
    /* Options to come:
     *   --device N        The GPU to use (default 0)
     *   --nic NAME        The RDMA device to use (default HIPOBJ_NIC_HINT,
     *                     or the one nearest the GPU)
     *   --host-only       Skip the GPU (gpuDevice = -1). Needs a NIC name.
     *   --size BYTES      The size of the buffers (default 1 MiB)
     *   --install-only    Stop after Tier A's first three steps, without
     *                     calling hipObjInit(), so the result doesn't
     *                     depend on the hardware
     *   --require gpu,rdma
     *                     Treat a SKIP for lack of that hardware as a
     *                     failure
     */
    const std::span<char *>        argvSpan = hipObj::argSpan(argc, argv);
    const std::vector<std::string> args(argvSpan.begin(), argvSpan.end());
    const std::string              prog    = args.empty() ? "hipobj-test-install" : args[0];
    bool                           help    = false;
    bool                           version = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--help") {
            help = true;
        }
        else if (args[i] == "--version") {
            version = true;
        }
        else {
            fprintf(stderr, "hipobj-test-install: unknown argument %s\n", args[i].c_str());
            usage(stderr, prog);
            return 2;
        }
    }
    if (help) {
        usage(stdout, prog);
        return 0;
    }
    if (version) {
        const std::string hipObjVersion = hipObjGetVersionString();
        printf("hipobj-test-install (hipObject %s)\n", hipObjVersion.c_str());
        return 0;
    }

    /* Tier A: no GPU, no NIC, no root
     *
     * 1. Library and version. If the tool started, libhipobj and
     *    libamdhip64 were found. Print hipObjGetVersionString() and the
     *    path of the libhipobj.so.0 that was loaded (dladdr() on a
     *    hipObject function), so a stray copy elsewhere on the loader's
     *    search path shows up. Warn if the version differs from the one
     *    the tool was built with.
     * 2. libibverbs. dlopen() libibverbs.so.1 and list the RDMA devices
     *    (ibv_get_device_list()), since hipObjInit() returns the same
     *    error when libibverbs didn't load as when there's no device with
     *    the hinted name. Failing to load is a FAIL, with dlerror()'s
     *    text and a hint about libibverbs1. No devices is a SKIP, with a
     *    hint about ibverbs-providers, ibv_devices, and rdma link.
     * 3. HIP. hipGetDeviceCount(). No GPU is a SKIP. Any other HIP error
     *    is a FAIL, with a hint about the render and video groups.
     * 4. Initialization. hipObjInit() with the GPU (or -1 with
     *    --host-only) and the NIC name. Without an RDMA device, or
     *    without a GPU or a NIC name, check that it fails the way the
     *    system predicts, which passes and skips the rest:
     *    hipObjRdmaError with hipErrorNoDevice for no GPU and no NIC
     *    name, and hipObjNicNotFound for a GPU with no NIC near it. An
     *    unexpected result is a FAIL, with the likely causes.
     */

    /* Tier B: an RDMA device, without a GPU
     *
     * A real NIC, or Soft-RoCE (rdma_rxe) on any Ethernet interface.
     *
     * 5. Host buffer. Allocate page-aligned host memory and
     *    hipObjBufRegisterHost().
     * 6. Tokens. hipObjGetRdmaToken() for each registered buffer,
     *    hipObjTokenClientNic() to report the NIC address the token
     *    carries, and hipObjPutRdmaToken(). Never print a token: it
     *    carries rkeys and addresses. Print its length and the NIC
     *    address only.
     * 7. Transfer setup. hipObjGet() on each registered buffer, with a
     *    sendRequest callback that records that it got a token and then
     *    fails, and a recvReply callback that should never be called.
     *    This leases one of the pool's connections, as every transfer
     *    does. Pass when sendRequest ran with a token and the result is
     *    hipObjS3Error: "transfer setup OK; no server, so no data moved".
     */

    /* Tier C: a GPU and an RDMA NIC
     *
     * GPU buffer, before step 5. hipMalloc() and hipObjBufRegister().
     * Call out hipObjDmabufNotSupported with a hint about the kernel and
     * driver. Steps 6 and 7 then run on the GPU buffer too.
     */

    /* Cleanup, whatever tier the run reached: deregister the buffers and
     * hipObjShutdown(). Each has to succeed.
     *
     * Then a one-line summary of how far it got: that hipObject is
     * installed and working on the GPU and NIC, that it's installed and
     * wired up but stopped at a tier for lack of hardware, or the step
     * that failed.
     */

    fprintf(stderr, "hipobj-test-install: the checks aren't implemented yet\n");
    return 1;
}
