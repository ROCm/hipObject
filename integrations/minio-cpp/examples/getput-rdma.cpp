/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* AMD port of minio-cpp GetPutRDMA: PUT + GET over hipObject RDMA */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <hip/hip_runtime_api.h>

#include <miniocpp/args.h>
#include <miniocpp/error.h>
#include <miniocpp/providers.h>
#include <miniocpp/request.h>
#include <miniocpp/response.h>
#include <unistd.h>

#include "hipobj-parse.h"
#include "hipobj-span.h"
#include "hipobj_minio/client.h"

namespace {

/* The positions of the optional arguments in argv */
constexpr size_t kSizeArg = 4;
constexpr size_t kGpuArg  = 5;

/* The size of the object when the arguments don't give one: 10 MiB */
constexpr size_t kDefaultBufSize = size_t{10} * 1024 * 1024;

/* What the buffer is filled with between the PUT and the GET */
constexpr char kClobberByte = 0x55;

/* PatternByte()'s constants: Knuth's MMIX LCG multiplier, and the
 * multiplier and shift of MurmurHash3's 64-bit finalizer, which mixes the
 * LCG's bits so every bit of the offset reaches the low byte */
constexpr uint64_t kLcgMultiplier = 6364136223846793005ULL;
constexpr uint64_t kMixMultiplier = 0xff51afd7ed558ccdULL;
constexpr unsigned kMixShift      = 33;

/* A position-dependent pattern, not a constant fill.
 *
 * This used to memset the buffer to 'A' and check afterwards that every byte
 * was still 'A', which cannot distinguish a correct transfer from a
 * byte-swapped, misaligned, short, duplicated or scattered one -- every one
 * of those returns a buffer full of 'A'. A cheap LCG keyed on the offset
 * makes each byte depend on where it is, so any of those failures shows up
 * as a mismatch at a specific offset, which is also the first thing you want
 * to know when it does. */
uint8_t
PatternByte(size_t offset, uint32_t seed)
{
    uint64_t x = ((offset + 1ULL) * kLcgMultiplier) + seed;
    x ^= x >> kMixShift;
    x *= kMixMultiplier;
    x ^= x >> kMixShift;
    /* The low byte */
    return static_cast<uint8_t>(x);
}

void
FillPattern(std::span<char> buf, uint32_t seed)
{
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<char>(PatternByte(i, seed));
    }
}

/* Returns the offset of the first mismatch, or buf.size() if the blob read
 * back matches the blob written byte for byte. */
size_t
FirstMismatch(std::span<const char> buf, uint32_t seed)
{
    for (size_t i = 0; i < buf.size(); ++i) {
        if (static_cast<uint8_t>(buf[i]) != PatternByte(i, seed)) {
            return i;
        }
    }
    return buf.size();
}

/* Frees the transfer buffer with the allocator that made it: hipFree() on
 * the GPU path, and on the host path, the aligned operator delete that
 * matches the aligned operator new */
struct BufferDeleter {
    bool             onGpu     = false;
    std::align_val_t alignment = {};

    void operator()(char *ptr) const noexcept
    {
        if (onGpu) {
            (void)hipFree(ptr);
        }
        else {
            ::operator delete(ptr, alignment);
        }
    }
};

} // namespace

int
main(int argc, char *argv[])
{
    const std::span<char *>        argv_span = hipObj::argSpan(argc, argv);
    const std::vector<std::string> args(argv_span.begin(), argv_span.end());
    if (args.size() < 4) {
        std::cerr << "usage: " << (args.empty() ? "minio-getput-rdma" : args[0])
                  << " <host> <access_key> <secret_key> [size_bytes] [gpu]\n";
        return 1;
    }

    const std::string &host        = args[1];
    const std::string &access_key  = args[2];
    const std::string &secret_key  = args[3];
    size_t             bufsize     = kDefaultBufSize;
    bool               gpu_enabled = true;

    if (args.size() > kSizeArg) {
        /* parseNumber() rejects signs, trailing junk, and values that don't
         * fit in size_t, all of which atol() would quietly accept. */
        const std::optional<size_t> parsed = hipObj::parseNumber<size_t>(args[kSizeArg]);
        if (!parsed) {
            std::cerr << "invalid size_bytes: " << args[kSizeArg] << "\n";
            return 1;
        }
        bufsize = *parsed;
    }
    if (args.size() > kGpuArg) {
        gpu_enabled = args[kGpuArg] == "gpu";
    }

    /* Seeded per run, and printed, so a stale object left in the store by an
     * earlier run cannot satisfy the readback. */
    const uint32_t seed = static_cast<uint32_t>(::getpid()) ^ static_cast<uint32_t>(std::time(nullptr));
    std::cout << "Pattern seed " << seed << "\n";

    std::vector<char> expected(bufsize);
    FillPattern(expected, seed);

    const minio::s3::BaseUrl     base_url(host, false, "us-east-1");
    minio::creds::StaticProvider provider(access_key, secret_key);
    hipobj::minio::Client        client(base_url, &provider);

    char *bufptr  = nullptr;
    void *dev_ptr = nullptr;
    /* bufptr's bytes, on the host path */
    std::span<char> host_buf;
    /* Owns bufptr (which is dev_ptr on the GPU path) */
    std::unique_ptr<char, BufferDeleter> buffer(nullptr, BufferDeleter{.onGpu = gpu_enabled});

    if (gpu_enabled) {
        hipError_t err = hipMalloc(&dev_ptr, bufsize);
        if (err != hipSuccess) {
            std::cerr << "hipMalloc failed: " << hipGetErrorString(err) << "\n";
            return 1;
        }
        buffer.reset(static_cast<char *>(dev_ptr));
        err = hipMemcpy(dev_ptr, expected.data(), bufsize, hipMemcpyHostToDevice);
        if (err != hipSuccess) {
            std::cerr << "hipMemcpy H2D failed: " << hipGetErrorString(err) << "\n";
            return 1;
        }
        (void)hipDeviceSynchronize();
        bufptr = static_cast<char *>(dev_ptr);
        std::cout << "GPU buffer " << bufsize << " bytes\n";
    }
    else {
        /* sysconf() returns a long */
        // NOLINTNEXTLINE(google-runtime-int)
        const long pagesize = sysconf(_SC_PAGESIZE);
        if (pagesize <= 0) {
            std::cerr << "sysconf(_SC_PAGESIZE) failed\n";
            return 1;
        }
        const auto alignment = std::align_val_t{static_cast<size_t>(pagesize)};
        bufptr               = static_cast<char *>(::operator new(bufsize, alignment, std::nothrow));
        if (bufptr == nullptr) {
            std::cerr << "allocating the host buffer failed\n";
            return 1;
        }
        buffer.get_deleter().alignment = alignment;
        buffer.reset(bufptr);
        host_buf = hipObj::spanFromPointer(bufptr, bufsize);
        std::ranges::copy(expected, host_buf.begin());
        std::cout << "Host buffer " << bufsize << " bytes\n";
    }

    minio::s3::PutObjectArgs pargs;
    pargs.buf    = bufptr;
    pargs.size   = bufsize;
    pargs.bucket = "hipobj-test";
    pargs.object = "my-object";

    const minio::s3::PutObjectResponse presp = client.PutObject(pargs);
    if (!presp) {
        std::cerr << "PUT failed: " << presp.Error().String() << "\n";
        return 1;
    }
    /* Flushed, so the line reaches the log even if the GET hangs and the
     * process is killed */
    std::cout << "PUT ok etag=" << presp.etag << "\n" << std::flush;

    /* Clobber the buffer before the GET. Without this the readback could be
     * satisfied by whatever the PUT left behind, and a GET that transferred
     * nothing at all would still verify. */
    if (gpu_enabled) {
        (void)hipMemset(dev_ptr, kClobberByte, bufsize);
        (void)hipDeviceSynchronize();
    }
    else {
        std::ranges::fill(host_buf, kClobberByte);
    }

    minio::s3::GetObjectArgs gargs;
    gargs.buf    = bufptr;
    gargs.size   = bufsize;
    gargs.bucket = "hipobj-test";
    gargs.object = "my-object";

    const minio::s3::GetObjectResponse gresp = client.GetObject(gargs);
    if (!gresp) {
        std::cerr << "GET failed: " << gresp.Error().String() << "\n";
        return 1;
    }
    std::cout << "GET ok\n";

    std::vector<char> hostbuf(bufsize);

    if (gpu_enabled) {
        const hipError_t err = hipMemcpy(hostbuf.data(), dev_ptr, bufsize, hipMemcpyDeviceToHost);
        if (err != hipSuccess) {
            std::cerr << "hipMemcpy failed: " << hipGetErrorString(err) << "\n";
            return 1;
        }
    }
    else {
        std::ranges::copy(host_buf, hostbuf.begin());
    }

    std::ofstream out("output.bin", std::ios::binary);
    if (!out) {
        std::cerr << "failed to open output.bin\n";
        return 1;
    }
    out.write(hostbuf.data(), static_cast<std::streamsize>(bufsize));
    out.close();
    std::cout << "Wrote output.bin (" << bufsize << " bytes)\n";

    const size_t bad = FirstMismatch(hostbuf, seed);
    const bool   ok  = bad == bufsize;
    if (ok) {
        std::cout << "Data integrity check passed (" << bufsize << " bytes, seed " << seed << ")\n";
    }
    else {
        std::cout << "Data integrity check FAILED at offset " << bad << ": expected 0x" << std::hex
                  << static_cast<int>(PatternByte(bad, seed)) << " got 0x"
                  << static_cast<int>(static_cast<uint8_t>(hostbuf[bad])) << std::dec << "\n";
    }

    /* Whether the transfers actually used RDMA. The client falls back to
     * ordinary HTTP silently when no NIC is available, so without this a lane
     * that is meant to be exercising the RDMA data path passes over TCP. */
    std::cout << hipobj::minio::TransferStatsLine(hipobj::minio::TransferStatsSnapshot()) << "\n";

    return ok ? 0 : 1;
}
