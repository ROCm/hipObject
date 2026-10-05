/* SPDX-License-Identifier: MIT
 *
 * Smoke test / usage example for HipObjAwsS3Client: PUTs a GPU buffer to
 * an object, then GETs it back, both via hipObject RDMA.
 *
 * Usage:
 *   hipobj-aws-s3-demo <bucket> <key> <size-bytes> [endpoint]
 *
 * If <endpoint> is omitted, it is picked up the same way the AWS CLI
 * does: from endpoint_url in the active profile of ~/.aws/config (or
 * AWS_ENDPOINT_URL / AWS_ENDPOINT_URL_S3), selected via AWS_PROFILE.
 * Point it at hipobj-rdma-test-server (see
 * ../rdma-test-server/run-v2-e2e.sh for how to launch one) or any other
 * S3-compatible endpoint that speaks the hipobj-rc-v1 header protocol.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

#ifdef HIPOBJ_CPU_RDMA
// No ROCm/HIP runtime on this target (regular NIC RDMA only, e.g.
// Mellanox mlx5) -- back the "device" buffer with plain host memory
// instead of a real GPU allocation.
namespace {
constexpr int hipSuccess = 0;
inline int hipMalloc(void** ptr, size_t size) {
  *ptr = std::malloc(size);
  return *ptr ? hipSuccess : 1;
}
inline int hipFree(void* ptr) {
  std::free(ptr);
  return hipSuccess;
}
} // namespace
#else
#include <hip/hip_runtime_api.h>
// A plain NIC (tcp mode) can only ever land bytes in host DRAM; it has no
// way to DMA into VRAM. On a host without Resizable BAR, wrapping devPtr
// (real GPU memory) directly in an HTTP body/response stream would fault.
// On a host *with* Resizable BAR it doesn't fault -- the whole VRAM range
// is mapped into host address space -- but every byte then goes over that
// mapping as an uncached PCIe MMIO access instead of a real DMA burst,
// which measured ~10x slower than staging through DRAM first (64 MB PUT:
// 4941 ms direct-through-BAR vs 472 ms via a host buffer). tcp mode below
// always stages through a pinned host buffer with an explicit hipMemcpy
// for this reason -- see HIPOBJ_DEMO_TCP_STAGE_GPU to disable and
// reproduce the direct-pointer behavior for comparison.
#define HIPOBJ_DEMO_HAS_REAL_HIP 1
#endif

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/EndpointResolver.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>

#include "hipobj_aws_s3_client.h"

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// Default size of the one-time-registered RDMA buffer (see
// RegisterBuffer()'s doc comment in hipobj_aws_s3_client.h): large enough
// to cover every size in a typical size-sweep comparison (4 KB .. 128 MB)
// with room to spare, so a single hipMalloc + hipObjBufRegister up front
// backs every PUT/GET this process makes. Override via
// HIPOBJ_DEMO_MR_CAPACITY (bytes) to test larger objects.
constexpr size_t kDefaultMrCapacity = 1ull << 30; // 1 GiB

// hipObj's own hard cap (hipobj-private.h's MAX_MR_SIZE) on a single
// hipObjBufRegister() call, not exposed outside the library -- matched here
// so parallel mode can split a batch across multiple <=this-size pools
// instead of hitting "RegisterBuffer failed: Size too large" once
// objSize*parallelCount exceeds it. Override via HIPOBJ_DEMO_POOL_BYTES.
constexpr size_t kMaxPoolBytes = 4ull << 30; // 4 GiB

size_t ResolveMrCapacity(size_t minCapacity) {
  size_t capacity = kDefaultMrCapacity;
  if (const char* env = std::getenv("HIPOBJ_DEMO_MR_CAPACITY")) {
    if (env[0] != '\0') {
      capacity = static_cast<size_t>(std::atoll(env));
    }
  }
  return capacity < minCapacity ? minCapacity : capacity;
}

// Number of times to repeat the PUT (or GET) call in a row against the
// same warm connection/registered buffer -- see HIPOBJ_DEMO_PUT_REPEAT /
// HIPOBJ_DEMO_GET_REPEAT. 1 keeps the original single-call behavior. In
// rdma mode "warm" means the listener and MR registration are reused
// (the RDMA CM connection itself is still re-established by the server
// on every call -- see the loop's comment below); in tcp mode it means
// the underlying HTTP client/connection is reused, which the AWS SDK
// already does via keep-alive across calls on the same client instance.
size_t ResolveRepeatCount(const char* envVar) {
  if (const char* env = std::getenv(envVar)) {
    long val = std::atol(env);
    if (val > 0) {
      return static_cast<size_t>(val);
    }
  }
  return 1;
}

// Fast (xorshift64*, ~word-at-a-time) fill so timing this doesn't itself
// become a bottleneck at multi-GB sizes -- real, non-degenerate content
// instead of the buffer's default all-zero state (fresh malloc()/hipMalloc()
// pages read back as zero regardless of whether a transfer even happened,
// which made an earlier real bug invisible: see rdma_client.cpp's chunking
// fix notes). Also matters now that the server bounces PUT/GET through a
// pre-registered pool via memcpy -- an all-zero source is not a realistic
// memcpy/cache workload.
void FillRandomData(void* ptr, size_t size) {
  uint64_t state = 0x243F6A8885A308D3ULL;
  auto* words = reinterpret_cast<uint64_t*>(ptr);
  size_t numWords = size / sizeof(uint64_t);
  for (size_t i = 0; i < numWords; ++i) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    words[i] = state * 0x2545F4914F6CDD1DULL;
  }
  size_t doneBytes = numWords * sizeof(uint64_t);
  if (doneBytes < size) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    uint64_t last = state * 0x2545F4914F6CDD1DULL;
    std::memcpy(reinterpret_cast<unsigned char*>(ptr) + doneBytes, &last, size - doneBytes);
  }
}

// Simple FNV-1a over `size` bytes -- diagnostic-only content check for
// verifying the server's pipelined disk-read + RDMA-write GET path
// actually reproduces the exact bytes that were PUT, not just that the
// control-plane round trip reported success.
uint64_t Fnv1aHash(const void* ptr, size_t size) {
  uint64_t h = 0xcbf29ce484222325ULL;
  const auto* bytes = reinterpret_cast<const unsigned char*>(ptr);
  for (size_t i = 0; i < size; ++i) {
    h ^= bytes[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

void PrintRepeatSummary(const char* label, const std::vector<double>& ms) {
  double sum = 0.0, lo = ms.front(), hi = ms.front();
  for (double v : ms) {
    sum += v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  std::fprintf(stdout,
              "%s over %zu warm-connection repeats: avg=%.3f ms min=%.3f "
              "ms max=%.3f ms (first call, incl. connect: %.3f ms)\n",
              label, ms.size(), sum / ms.size(), lo, hi, ms.front());
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::fprintf(stderr,
                "Usage: %s <bucket> <key> <size-bytes> [endpoint] [mode: rdma|tcp]\n"
                "  mode defaults to rdma; tcp does the same PUT+GET pair as a\n"
                "  plain HTTP body transfer on the same client/connection, for\n"
                "  a fair same-process timing comparison against rdma mode.\n"
                "  rdma mode registers a %llu-byte buffer once up front\n"
                "  (override via HIPOBJ_DEMO_MR_CAPACITY) and reuses it for\n"
                "  both PUT and GET, so the reported PutObjectRdma/\n"
                "  GetObjectRdma timings measure only the RDMA transfer,\n"
                "  not memory registration.\n"
                "  HIPOBJ_DEMO_PUT_REPEAT / HIPOBJ_DEMO_GET_REPEAT (default\n"
                "  1) repeat PUT/GET N times against the same warm\n"
                "  connection/buffer and print avg/min/max.\n"
                "  HIPOBJ_DEMO_PARALLEL (default 1, rdma mode only) runs N\n"
                "  concurrent PUT/GET calls via N threads, against N objects.\n"
                "  HIPOBJ_DEMO_PARALLEL_INFLIGHT (default: same as PARALLEL)\n"
                "  caps how many run at once; the rest queue.\n"
                "  HIPOBJ_DEMO_PARALLEL_THREADS (default: same as PARALLEL)\n"
                "  uses fewer worker threads, each stepping through multiple\n"
                "  objects sequentially instead of one thread per object --\n"
                "  measures sustained aggregate throughput from a fixed\n"
                "  number of concurrent streams.\n"
                "  HIPOBJ_DEMO_SKIP_PUT / HIPOBJ_DEMO_SKIP_GET (default 0)\n"
                "  skip the PUT or GET phase, to create objects once then\n"
                "  push GET separately at a different concurrency.\n"
                "  HIPOBJ_DEMO_TCP_STAGE_GPU (default 1, real-HIP builds only)\n"
                "  stages tcp mode's HTTP body/response through a pinned host\n"
                "  buffer with an explicit hipMemcpy instead of pointing\n"
                "  libcurl at GPU memory directly. Set to 0 to reproduce the\n"
                "  direct-pointer behavior (only viable, and ~10x slower on\n"
                "  the write side, on hosts with Resizable BAR).\n"
                "  HIPOBJ_DEMO_RDMA_STAGE_DRAM (default 0, real-HIP builds\n"
                "  only) forces rdma mode to stage through the same pinned\n"
                "  host buffer and hipMemcpy to/from the real GPU buffer,\n"
                "  instead of real GPU-direct RDMA on devPtr itself. For\n"
                "  measuring the fair cost when the application's data lives\n"
                "  on the GPU but RDMA only registers a DRAM buffer.\n",
                argv[0], static_cast<unsigned long long>(kDefaultMrCapacity));
    return 1;
  }
  const char* bucket = argv[1];
  const char* key = argv[2];
  size_t objSize = static_cast<size_t>(std::atol(argv[3]));
  if (objSize == 0) {
    std::fprintf(stderr, "Invalid object size: %s\n", argv[3]);
    return 1;
  }
  const char* endpoint = argc > 4 ? argv[4] : nullptr;
  const char* mode = argc > 5 ? argv[5] : "rdma";
  bool tcpMode = std::strcmp(mode, "tcp") == 0;
  if (!tcpMode && std::strcmp(mode, "rdma") != 0) {
    std::fprintf(stderr, "Invalid mode: %s (expected rdma or tcp)\n", mode);
    return 1;
  }
  /* HIPOBJ_DEMO_PARALLEL > 1: spawns worker threads, each PUTting/GETting
   * objSize bytes at its own offset into one shared buffer, instead of the
   * sequential/repeat path. rdma mode registers that buffer (split across
   * pools, see kMaxPoolBytes below) and uses PutObjectRdma/GetObjectRdma's
   * offset param; tcp mode needs no registration and just wraps each
   * thread's slice in its own PreallocatedStreamBuf via the normal SDK
   * PutObject/GetObject calls, relying on the SDK's HTTP connection pool
   * (see HIPOBJ_DEMO_MAX_CONNECTIONS) for real concurrency. */
  size_t parallelCount = ResolveRepeatCount("HIPOBJ_DEMO_PARALLEL");

  Aws::SDKOptions options;
  Aws::InitAPI(options);
  int exitCode = 0;
  {
    Aws::S3::S3ClientConfiguration config;
    config.region = "us-east-1";
    // Default is 25 -- each PutObjectRdma/GetObjectRdma control-plane
    // exchange (the tiny HTTP request carrying x-amz-rc-rdma-token) still
    // goes through the SDK's normal pooled HTTP client even though the
    // actual data moves out-of-band via RDMA, so with HIPOBJ_DEMO_PARALLEL
    // above ~25 the excess threads queue for a pooled connection instead of
    // actually running concurrently -- but that queueing is what keeps the
    // server's single-threaded HTTP accept loop from being hit with more
    // simultaneous fresh connections than it can accept(); raising this
    // (HIPOBJ_DEMO_MAX_CONNECTIONS) past roughly the server's accept
    // throughput trades graceful client-side queueing for real
    // connect()-timeout failures under HIPOBJ_DEMO_PARALLEL. Left at the
    // SDK default unless explicitly overridden.
    if (const char* env = std::getenv("HIPOBJ_DEMO_MAX_CONNECTIONS")) {
      long v = std::atol(env);
      if (v > 0) {
        config.maxConnections = static_cast<unsigned>(v);
      }
    }
    // The RC-CM handshake for the RDMA control request can take a few
    // retries to reach ESTABLISHED on a cold connection (RDMA CM's own
    // REQ retransmission), longer than the SDK's 3s default
    // requestTimeoutMs/low-speed-time -- give it more room.
    config.requestTimeoutMs = 20000;
    // For focused debugging: one attempt only no SDK-level HTTP retries
    // keeps a single connection attempt traceable end-to-end in packet capture.
    config.retryStrategy = Aws::Client::InitRetryStrategy(1);
    // The actual object content never passes through the SDK -- it's
    // transferred out-of-band via RDMA, so the SDK has nothing but an empty
    // body to checksum for this control request. With the default
    // WHEN_SUPPORTED, the SDK auto-attaches an x-amz-checksum-* header
    // computed over that empty body, which the server then correctly
    // rejects as a BadDigest mismatch against the real RDMA-received data.
    // WHEN_REQUIRED skips that for PutObject, where it isn't mandatory.
    config.checksumConfig.requestChecksumCalculation =
      Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;

    // HipObjAwsS3Client's RDMA control path builds its request URL
    // itself from clientConfiguration.endpointOverride (see its
    // constructor) rather than going through Aws::Client::AWSClient's
    // normal HTTP path, so it never sees the SDK's own built-in
    // endpoint-resolution machinery -- an empty endpointOverride here
    // would reach it as an empty host. Resolve explicitly with the
    // same logic the SDK itself uses (env vars, then endpoint_url in
    // the active ~/.aws/config profile) so both paths end up with the
    // same endpoint.
    Aws::String resolvedEndpoint = endpoint ? Aws::String(endpoint) :
      Aws::Config::EndpointResolver::EndpointSource("s3", config.profileName);
    if (resolvedEndpoint.empty()) {
      std::fprintf(stderr,
                  "No endpoint given and none configured (pass one, or set "
                  "endpoint_url in ~/.aws/config or AWS_ENDPOINT_URL)\n");
      Aws::ShutdownAPI(options);
      return 1;
    }
    config.endpointOverride = resolvedEndpoint;
    config.scheme = strncmp(resolvedEndpoint.c_str(), "https", 5) == 0
                     ? Aws::Http::Scheme::HTTPS
                     : Aws::Http::Scheme::HTTP;

    // Explicit anonymous credentials instead of the config-only constructor
    // (which resolves via DefaultAWSCredentialsProviderChain): empty
    // access-key/secret makes AWSAuthV4Signer::SignRequestWithCreds() skip
    // SigV4 signing entirely ("don't sign anonymous requests"), which also
    // skips the real SHA256 hash it otherwise computes over the whole
    // payload for x-amz-content-sha256 on every non-HTTPS request -- a
    // second full read of the (possibly just-staged-from-GPU) buffer that
    // has nothing to do with the RDMA-vs-TCP transport comparison this demo
    // is for. simple-s3-server never validates the Authorization header, so
    // this is free here; a real S3-compatible endpoint that checks
    // signatures would reject unsigned requests -- AWS_ACCESS_KEY_ID/
    // AWS_SECRET_ACCESS_KEY in the environment opt into real SigV4 signing
    // (paying the extra SHA256 pass) for that case.
    const char* accessKeyId = std::getenv("AWS_ACCESS_KEY_ID");
    const char* secretKey = std::getenv("AWS_SECRET_ACCESS_KEY");
    Aws::Auth::AWSCredentials credentials =
      (accessKeyId && secretKey)
        ? Aws::Auth::AWSCredentials(accessKeyId, secretKey)
        : Aws::Auth::AWSCredentials("", "");
    hipobj::aws::HipObjAwsS3Client client(credentials, config);

    // rdma mode allocates and registers a single buffer sized to cover
    // objSize up front (see kDefaultMrCapacity above) so PutObjectRdma()/
    // GetObjectRdma() below reuse an already-registered pointer instead of
    // each paying its own ibv_reg_mr() -- see RegisterBuffer()'s doc
    // comment in hipobj_aws_s3_client.h. tcp mode never registers memory,
    // but in parallel mode (parallelCount > 1) still needs room for every
    // object's own slice of the buffer, same as rdma mode -- sizing this
    // at objSize alone (pre-parallel-mode behavior) let concurrent TCP
    // threads read/write past the single-object allocation.
    size_t allocSize = (tcpMode && parallelCount <= 1)
      ? objSize
      : ResolveMrCapacity(objSize * std::max<size_t>(parallelCount, 1));
    void* devPtr = nullptr;
    if (hipMalloc(&devPtr, allocSize) != hipSuccess) {
      std::fprintf(stderr, "hipMalloc failed\n");
      Aws::ShutdownAPI(options);
      return 1;
    }

    // Diagnostic-only: confirm devPtr is actually backed by GPU VRAM (not
    // just a host pointer we assume is device memory) by reading HIP's own
    // free/total VRAM accounting right after the allocation. Off by default
    // and gated behind an explicit opt-in env var so it never runs during a
    // benchmark -- hipMemGetInfo() is a device-synchronizing driver call and
    // would skew timing if left on.
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
    if (const char* env = std::getenv("HIPOBJ_DEMO_LOG_GPU_MEM")) {
      if (env[0] != '0') {
        size_t freeBytes = 0, totalBytes = 0;
        if (hipMemGetInfo(&freeBytes, &totalBytes) == hipSuccess) {
          std::fprintf(stderr,
                       "hipMemGetInfo after hipMalloc(%zu): free=%zu total=%zu "
                       "(used=%zu) devPtr=%p\n",
                       allocSize, freeBytes, totalBytes, totalBytes - freeBytes, devPtr);
        }
      }
    }
#endif

    // tcp mode's HTTP body/response streams need CPU-accessible memory.
    // devPtr is real GPU memory on a HIPOBJ_DEMO_HAS_REAL_HIP build (see
    // the comment on that macro above), so stage through a pinned host
    // buffer with an explicit hipMemcpy instead of pointing libcurl at
    // devPtr directly. Opt out with HIPOBJ_DEMO_TCP_STAGE_GPU=0 to
    // reproduce the direct (Resilient-BAR-dependent, ~10x slower) path.
    //
    // rdma mode normally registers devPtr itself (real GPU-direct RDMA,
    // no copy at all). HIPOBJ_DEMO_RDMA_STAGE_DRAM=1 forces rdma mode to
    // stage through this same pinned host buffer instead -- i.e.
    // registers/transfers the DRAM buffer over RDMA and pays an explicit
    // hipMemcpy to/from the real GPU buffer, bracketing the RDMA call the
    // same way tcp mode's staging does. This exists to make a fair
    // "the application's data actually lives on the GPU" comparison
    // against GPU-direct RDMA: it is off by default so a plain rdma run
    // keeps doing real GPU-direct RDMA unchanged.
    void* hostStageBuf = nullptr;
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
    bool stageGpuForTcp = tcpMode;
    if (const char* env = std::getenv("HIPOBJ_DEMO_TCP_STAGE_GPU")) {
      stageGpuForTcp = tcpMode && env[0] != '0';
    }
    bool stageGpuForRdma = false;
    if (!tcpMode) {
      if (const char* env = std::getenv("HIPOBJ_DEMO_RDMA_STAGE_DRAM")) {
        stageGpuForRdma = env[0] != '0';
      }
    }
    if (stageGpuForTcp || stageGpuForRdma) {
      if (hipHostMalloc(&hostStageBuf, allocSize, hipHostMallocDefault) != hipSuccess) {
        std::fprintf(stderr, "hipHostMalloc (DRAM staging buffer) failed\n");
        (void)hipFree(devPtr);
        Aws::ShutdownAPI(options);
        return 1;
      }
    }
#endif
    // The buffer actually handed to the transport: the DRAM staging
    // buffer when staging (tcp mode by default, rdma mode only with
    // HIPOBJ_DEMO_RDMA_STAGE_DRAM=1), devPtr otherwise -- real GPU-direct
    // RDMA, or a CPU_RDMA build where devPtr already is host memory.
    void* const ioBuf = hostStageBuf ? hostStageBuf : devPtr;

    {
      auto fillStart = Clock::now();
      FillRandomData(devPtr, allocSize);
      std::fprintf(stdout,
                  "One-time random data fill for %zu bytes took %.3f ms "
                  "(excluded from PUT/GET timings below)\n",
                  allocSize, ElapsedMs(fillStart, Clock::now()));
      if (const char* verify = std::getenv("HIPOBJ_DEMO_VERIFY_CHECKSUM")) {
        (void)verify;
        std::fprintf(stdout, "Pre-PUT checksum (first %zu bytes): %016llx\n", objSize,
                    static_cast<unsigned long long>(Fnv1aHash(devPtr, objSize)));
      }
    }

    // parallelCount > 1 registers as multiple <=kMaxPoolBytes pools instead
    // (see below) -- a single objSize*parallelCount registration would hit
    // hipObj's hard MAX_MR_SIZE cap (4 GiB) well before GPU/DRAM capacity
    // does, e.g. 64 objects x 128 MiB = 8 GiB.
    if (!tcpMode && parallelCount <= 1) {
      auto regStart = Clock::now();
      hipObjError_t regErr = client.RegisterBuffer(ioBuf, allocSize);
      auto regEnd = Clock::now();
      if (regErr.opError != hipObjSuccess) {
        std::fprintf(stderr, "RegisterBuffer failed: %s\n",
                    hipObjGetErrorString(regErr.opError));
        (void)hipFree(devPtr);
        Aws::ShutdownAPI(options);
        return 1;
      }
      std::fprintf(stdout,
                  "One-time MR registration for %zu bytes took %.3f ms "
                  "(excluded from PUT/GET timings below)\n",
                  allocSize, ElapsedMs(regStart, regEnd));
    }

    // Both modes now repeat PUT (HIPOBJ_DEMO_PUT_REPEAT) and GET
    // (HIPOBJ_DEMO_GET_REPEAT) against the one client/listener/buffer set
    // up above -- process startup, SDK init, listener startup and (rdma
    // only) MR registration are all paid exactly once, before any of
    // these calls. In rdma mode the server still dials a fresh RDMA CM
    // connection per PUT/GET regardless of repeat count (see rc-listener
    // DEBUG logs -- each call gets its own CONNECT_REQUEST/QP), so repeat
    // numbers isolate per-call RDMA CM handshake + control-request cost
    // from one-time process/listener/registration cost, not a literal
    // reused RDMA connection. In tcp mode the AWS SDK's own HTTP
    // keep-alive already reuses the underlying connection across calls on
    // one client instance, with or without this loop.
    size_t putRepeat = ResolveRepeatCount("HIPOBJ_DEMO_PUT_REPEAT");
    size_t getRepeat = ResolveRepeatCount("HIPOBJ_DEMO_GET_REPEAT");

    const char* putLabel = tcpMode ? "PutObject" : "PutObjectRdma";
    const char* getLabel = tcpMode ? "GetObject" : "GetObjectRdma";

    if (parallelCount > 1) {
      // Caps how many of the parallelCount calls run at once; the rest
      // queue. Lets objects be created at a safe concurrency, then read
      // back at a different one. Defaults to parallelCount (unthrottled).
      size_t maxInFlight = parallelCount;
      if (const char* env = std::getenv("HIPOBJ_DEMO_PARALLEL_INFLIGHT")) {
        long v = std::atol(env);
        if (v > 0) {
          maxInFlight = static_cast<size_t>(v);
        }
      }
      bool skipPut = false;
      if (const char* env = std::getenv("HIPOBJ_DEMO_SKIP_PUT")) {
        skipPut = env[0] != '0';
      }
      bool skipGet = false;
      if (const char* env = std::getenv("HIPOBJ_DEMO_SKIP_GET")) {
        skipGet = env[0] != '0';
      }
      // Number of worker threads. Default: one thread per object (each does
      // exactly one call, the original behavior). Set lower than
      // parallelCount to have each thread step through multiple objects
      // sequentially (round-robin striped) instead -- measures sustained
      // aggregate throughput from a fixed number of concurrent streams
      // rather than a one-shot all-at-once burst.
      size_t numThreads = parallelCount;
      if (const char* env = std::getenv("HIPOBJ_DEMO_PARALLEL_THREADS")) {
        long v = std::atol(env);
        if (v > 0) {
          numThreads = static_cast<size_t>(v);
        }
      }

      // Split the parallel batch across pools of <=poolBytes each. hipObj's
      // MAX_MR_SIZE cap applies per hipObjBufRegister() call, not to the
      // underlying hipMalloc/hipHostMalloc allocation, so devPtr/
      // hostStageBuf stay one big allocation -- only the registration (and
      // each RDMA call's base pointer) is split across pools, dedicating a
      // contiguous run of object indices to each pool.
      size_t poolBytes = kMaxPoolBytes;
      if (const char* env = std::getenv("HIPOBJ_DEMO_POOL_BYTES")) {
        long long v = std::atoll(env);
        if (v > 0) {
          poolBytes = static_cast<size_t>(v);
        }
      }
      if (objSize > poolBytes) {
        std::fprintf(stderr,
                    "objSize (%zu) exceeds HIPOBJ_DEMO_POOL_BYTES (%zu) -- a "
                    "single object must fit in one pool\n",
                    objSize, poolBytes);
        (void)hipFree(devPtr);
        Aws::ShutdownAPI(options);
        return 1;
      }
      size_t objectsPerPool = poolBytes / objSize;
      size_t numPools = (parallelCount + objectsPerPool - 1) / objectsPerPool;
      size_t poolStrideBytes = objectsPerPool * objSize;
      std::vector<void*> poolBase(numPools);
      if (!tcpMode) {
        for (size_t p = 0; p < numPools; ++p) {
          size_t objectsInPool = std::min(objectsPerPool, parallelCount - p * objectsPerPool);
          size_t regBytes = objectsInPool * objSize;
          poolBase[p] = reinterpret_cast<char*>(ioBuf) + p * poolStrideBytes;
          auto regStart = Clock::now();
          hipObjError_t regErr = client.RegisterBuffer(poolBase[p], regBytes);
          if (regErr.opError != hipObjSuccess) {
            std::fprintf(stderr, "RegisterBuffer failed for pool %zu/%zu (%zu bytes): %s\n",
                        p + 1, numPools, regBytes, hipObjGetErrorString(regErr.opError));
            for (size_t q = 0; q < p; ++q) {
              (void)client.DeregisterBuffer(poolBase[q]);
            }
            (void)hipFree(devPtr);
            Aws::ShutdownAPI(options);
            return 1;
          }
          std::fprintf(stdout,
                      "One-time MR registration for pool %zu/%zu (%zu bytes, "
                      "%zu objects) took %.3f ms (excluded from PUT/GET "
                      "timings below)\n",
                      p + 1, numPools, regBytes, objectsInPool,
                      ElapsedMs(regStart, Clock::now()));
        }
        std::fprintf(stdout,
                    "%zu objects split across %zu pool(s), up to %zu bytes / "
                    "%zu objects each\n",
                    parallelCount, numPools, poolBytes, objectsPerPool);
      }
      auto poolPtrFor = [&](size_t i) -> void* {
        return reinterpret_cast<char*>(ioBuf) + (i / objectsPerPool) * poolStrideBytes;
      };
      auto poolOffsetFor = [&](size_t i) -> off_t {
        return static_cast<off_t>((i % objectsPerPool) * objSize);
      };

      // Counting semaphore (std::counting_semaphore is C++20; this target
      // is C++17).
      struct Semaphore {
        std::mutex m;
        std::condition_variable cv;
        size_t available;
        explicit Semaphore(size_t n) : available(n) {}
        void Acquire() {
          std::unique_lock<std::mutex> lock(m);
          cv.wait(lock, [&] { return available > 0; });
          --available;
        }
        void Release() {
          {
            std::lock_guard<std::mutex> lock(m);
            ++available;
          }
          cv.notify_one();
        }
      } inFlightGate(maxInFlight);

      auto runParallel = [&](const char* label, bool isPut) -> bool {
        /* uint8_t, not bool -- std::vector<bool> is bit-packed, so
         * concurrent writes to different indices from different threads
         * is a real data race (can corrupt neighboring bits) even though
         * each thread touches a logically distinct element -- can
         * manifest as allOk=false below with no corresponding per-call
         * failure message ever printed. */
        std::vector<uint8_t> ok(parallelCount, 0);
        std::vector<double> ms(parallelCount, 0.0);
        auto start = Clock::now();
        {
          std::vector<std::thread> threads;
          for (size_t t = 0; t < numThreads; ++t) {
            threads.emplace_back([&, t]() {
              // Round-robin striped: with numThreads < parallelCount, this
              // thread steps through every parallelCount-th object
              // sequentially instead of each object getting its own thread.
              for (size_t i = t; i < parallelCount; i += numThreads) {
                inFlightGate.Acquire();
                struct ReleaseGuard {
                  Semaphore* gate;
                  ~ReleaseGuard() { gate->Release(); }
                } releaseGuard{&inFlightGate};
                Aws::String slotKey = Aws::String(key) + "-" + std::to_string(i);
                void* poolPtr = poolPtrFor(i);
                off_t offset = poolOffsetFor(i);
                unsigned char* slicePtr = reinterpret_cast<unsigned char*>(poolPtr) + offset;
                auto t0 = Clock::now();
                if (isPut) {
                  bool putOk;
                  Aws::String errMsg;
                  if (tcpMode) {
                    Aws::S3::Model::PutObjectRequest req;
                    req.SetBucket(bucket);
                    req.SetKey(slotKey);
                    Aws::Utils::Stream::PreallocatedStreamBuf putBuf(slicePtr, objSize);
                    req.SetBody(Aws::MakeShared<Aws::IOStream>("aws-s3-demo", &putBuf));
                    auto outcome = client.PutObject(req);
                    putOk = outcome.IsSuccess();
                    if (!putOk) errMsg = outcome.GetError().GetMessage();
                  } else {
                    Aws::S3::Model::PutObjectRequest req;
                    req.SetBucket(bucket);
                    req.SetKey(slotKey);
                    auto outcome = client.PutObjectRdma(req, poolPtr, objSize, offset);
                    putOk = outcome.IsSuccess();
                    if (!putOk) errMsg = outcome.GetError().GetMessage();
                  }
                  ok[i] = putOk;
                  if (!putOk) {
                    std::fprintf(stderr, "%s[%zu] failed: %s\n", label, i, errMsg.c_str());
                  }
                } else {
                  bool getOk;
                  Aws::String errMsg;
                  if (tcpMode) {
                    Aws::S3::Model::GetObjectRequest req;
                    req.SetBucket(bucket);
                    req.SetKey(slotKey);
                    req.SetResponseStreamFactory([slicePtr, objSize]() {
                      return Aws::New<Aws::IOStream>(
                        "aws-s3-demo",
                        new Aws::Utils::Stream::PreallocatedStreamBuf(slicePtr, objSize));
                    });
                    auto outcome = client.GetObject(req);
                    getOk = outcome.IsSuccess();
                    if (!getOk) errMsg = outcome.GetError().GetMessage();
                  } else {
                    Aws::S3::Model::GetObjectRequest req;
                    req.SetBucket(bucket);
                    req.SetKey(slotKey);
                    auto outcome = client.GetObjectRdma(req, poolPtr, objSize, offset);
                    getOk = outcome.IsSuccess();
                    if (!getOk) errMsg = outcome.GetError().GetMessage();
                  }
                  ok[i] = getOk;
                  if (!getOk) {
                    std::fprintf(stderr, "%s[%zu] failed: %s\n", label, i, errMsg.c_str());
                  }
                }
                ms[i] = ElapsedMs(t0, Clock::now());
              }
            });
          }
          for (auto& t : threads) {
            t.join();
          }
        }
        double totalMs = ElapsedMs(start, Clock::now());
        bool allOk = std::all_of(ok.begin(), ok.end(), [](bool v) { return v; });
        double minMs = *std::min_element(ms.begin(), ms.end());
        double maxMs = *std::max_element(ms.begin(), ms.end());
        double avgMs = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
        double gbits = parallelCount * objSize * 8.0 / (totalMs / 1000.0) / 1e9;
        std::fprintf(stdout,
                    "%s parallel[%zu objects/%zu threads]: total=%.3f ms "
                    "aggregate=%.2f Gb/s per-call min/avg/max=%.1f/%.1f/%.1f "
                    "ms all_ok=%s\n",
                    label, parallelCount, numThreads, totalMs, gbits, minMs,
                    avgMs, maxMs, allOk ? "true" : "false");
        return allOk;
      };

      // Always warms up (establishes/pools connections) regardless of
      // HIPOBJ_DEMO_SKIP_PUT, so a GET-only run against pre-existing
      // objects gets the same warm-connection-pool benefit a PUT run does
      // -- skipping this for GET-only previously meant every GET call paid
      // full cold RDMA CM handshake cost, giving numbers not comparable to
      // a PUT run's (which always warmed up first).
      bool warmOk = true;
      std::fprintf(stdout,
                  "=== warmup: %zu sequential small PUTs (establishes/pools "
                  "connections) ===\n",
                  parallelCount);
      for (size_t i = 0; i < parallelCount && warmOk; ++i) {
        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(Aws::String(key) + "-warmup-" + std::to_string(i));
        void* poolPtr = poolPtrFor(i);
        off_t offset = poolOffsetFor(i);
        size_t warmSize = std::min<size_t>(objSize, 4096);
        auto t0 = Clock::now();
        bool ok;
        if (tcpMode) {
          Aws::Utils::Stream::PreallocatedStreamBuf warmBuf(
            reinterpret_cast<unsigned char*>(poolPtr) + offset, warmSize);
          req.SetBody(Aws::MakeShared<Aws::IOStream>("aws-s3-demo", &warmBuf));
          ok = client.PutObject(req).IsSuccess();
        } else {
          ok = client.PutObjectRdma(req, poolPtr, warmSize, offset).IsSuccess();
        }
        double ms = ElapsedMs(t0, Clock::now());
        warmOk = ok;
        std::fprintf(stdout, "  warmup PUT %zu: %.1f ms%s\n", i, ms,
                    warmOk ? "" : " (FAILED)");
      }
      if (!skipPut) {
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
        if (warmOk && hostStageBuf) {
          auto cpyStart = Clock::now();
          hipError_t cpyErr = hipMemcpy(hostStageBuf, devPtr, allocSize, hipMemcpyDeviceToHost);
          std::fprintf(stdout,
                      "One-time bulk D2H stage for %zu-way parallel PUT (%zu bytes) took %.3f ms\n",
                      parallelCount, allocSize, ElapsedMs(cpyStart, Clock::now()));
          if (cpyErr != hipSuccess) {
            std::fprintf(stderr, "hipMemcpy (D2H stage for parallel PUT) failed: %s\n",
                        hipGetErrorString(cpyErr));
            warmOk = false;
          }
        }
#endif
        if (!warmOk || !runParallel(putLabel, /*isPut=*/true)) {
          exitCode = 1;
        }
      } else {
        std::fprintf(stdout, "=== skipping PUT (HIPOBJ_DEMO_SKIP_PUT) -- reading back "
                    "%zu pre-existing objects ===\n", parallelCount);
        if (!warmOk) {
          exitCode = 1;
        }
      }
      if (exitCode == 0 && !skipGet && !runParallel(getLabel, /*isPut=*/false)) {
        exitCode = 1;
      } else if (skipGet) {
        std::fprintf(stdout, "=== skipping GET (HIPOBJ_DEMO_SKIP_GET) ===\n");
      }
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
      if (exitCode == 0 && !skipGet && hostStageBuf) {
        auto cpyStart = Clock::now();
        hipError_t cpyErr = hipMemcpy(devPtr, hostStageBuf, allocSize, hipMemcpyHostToDevice);
        std::fprintf(stdout,
                    "One-time bulk H2D stage after %zu-way parallel GET (%zu bytes) took %.3f ms\n",
                    parallelCount, allocSize, ElapsedMs(cpyStart, Clock::now()));
        if (cpyErr != hipSuccess) {
          std::fprintf(stderr, "hipMemcpy (H2D stage after parallel GET) failed: %s\n",
                      hipGetErrorString(cpyErr));
          exitCode = 1;
        }
      }
#endif
      if (!tcpMode) {
        for (size_t p = 0; p < numPools; ++p) {
          hipObjError_t deregErr = client.DeregisterBuffer(poolBase[p]);
          if (deregErr.opError != hipObjSuccess) {
            std::fprintf(stderr, "DeregisterBuffer failed for pool %zu/%zu: %s\n",
                        p + 1, numPools, hipObjGetErrorString(deregErr.opError));
          }
        }
      }
      (void)hipFree(devPtr);
      Aws::ShutdownAPI(options);
      return exitCode;
    }

    std::vector<double> putTimesMs;
    putTimesMs.reserve(putRepeat);
    for (size_t i = 0; i < putRepeat && exitCode == 0; ++i) {
      double ms = 0.0;
      bool ok = false;
      Aws::String etag;
      if (tcpMode) {
        Aws::S3::Model::PutObjectRequest putReq;
        putReq.SetBucket(bucket);
        putReq.SetKey(key);
        Aws::Utils::Stream::PreallocatedStreamBuf putBuf(
          reinterpret_cast<unsigned char*>(ioBuf), objSize);
        putReq.SetBody(Aws::MakeShared<Aws::IOStream>("aws-s3-demo", &putBuf));

        auto putStart = Clock::now();
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
        if (hostStageBuf) {
          hipError_t cpyErr = hipMemcpy(hostStageBuf, devPtr, objSize, hipMemcpyDeviceToHost);
          if (cpyErr != hipSuccess) {
            std::fprintf(stderr, "hipMemcpy (D2H stage for PUT) failed: %s\n",
                        hipGetErrorString(cpyErr));
            exitCode = 1;
            break;
          }
        }
#endif
        auto putOutcome = client.PutObject(putReq);
        ms = ElapsedMs(putStart, Clock::now());
        ok = putOutcome.IsSuccess();
        if (ok) {
          etag = putOutcome.GetResult().GetETag();
        } else {
          std::fprintf(stderr, "PutObject failed on repeat %zu/%zu: %s\n",
                      i + 1, putRepeat,
                      putOutcome.GetError().GetMessage().c_str());
        }
      } else {
        Aws::S3::Model::PutObjectRequest putReq;
        putReq.SetBucket(bucket);
        putReq.SetKey(key);

        auto putStart = Clock::now();
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
        if (hostStageBuf) {
          hipError_t cpyErr = hipMemcpy(hostStageBuf, devPtr, objSize, hipMemcpyDeviceToHost);
          if (cpyErr != hipSuccess) {
            std::fprintf(stderr, "hipMemcpy (D2H stage for PutObjectRdma) failed: %s\n",
                        hipGetErrorString(cpyErr));
            exitCode = 1;
            break;
          }
        }
#endif
        auto putOutcome = client.PutObjectRdma(putReq, ioBuf, objSize);
        ms = ElapsedMs(putStart, Clock::now());
        ok = putOutcome.IsSuccess();
        if (ok) {
          etag = putOutcome.GetResult().GetETag();
        } else {
          std::fprintf(stderr, "PutObjectRdma failed on repeat %zu/%zu: %s\n",
                      i + 1, putRepeat,
                      putOutcome.GetError().GetMessage().c_str());
        }
      }
      if (!ok) {
        exitCode = 1;
        break;
      }
      putTimesMs.push_back(ms);
      if (putRepeat == 1) {
        std::fprintf(stdout, "%s succeeded for %zu bytes in %.3f ms (etag=%s)\n",
                    putLabel, objSize, ms, etag.c_str());
      } else {
        std::fprintf(stdout,
                    "%s[%zu/%zu] succeeded for %zu bytes in %.3f ms "
                    "(etag=%s)\n",
                    putLabel, i + 1, putRepeat, objSize, ms, etag.c_str());
      }
    }
    if (exitCode == 0 && putRepeat > 1) {
      PrintRepeatSummary(putLabel, putTimesMs);
    }

    std::vector<double> getTimesMs;
    getTimesMs.reserve(getRepeat);
    for (size_t i = 0; i < getRepeat && exitCode == 0; ++i) {
      double ms = 0.0;
      bool ok = false;
      if (tcpMode) {
        Aws::S3::Model::GetObjectRequest getReq;
        getReq.SetBucket(bucket);
        getReq.SetKey(key);
        getReq.SetResponseStreamFactory([ioBuf, objSize]() {
          return Aws::New<Aws::IOStream>(
            "aws-s3-demo",
            new Aws::Utils::Stream::PreallocatedStreamBuf(
              reinterpret_cast<unsigned char*>(ioBuf), objSize));
        });

        auto getStart = Clock::now();
        auto getOutcome = client.GetObject(getReq);
        ok = getOutcome.IsSuccess();
        if (!ok) {
          ms = ElapsedMs(getStart, Clock::now());
          std::fprintf(stderr, "GetObject failed on repeat %zu/%zu: %s\n",
                      i + 1, getRepeat,
                      getOutcome.GetError().GetMessage().c_str());
        } else {
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
          if (hostStageBuf) {
            hipError_t cpyErr = hipMemcpy(devPtr, hostStageBuf, objSize, hipMemcpyHostToDevice);
            if (cpyErr != hipSuccess) {
              ok = false;
              std::fprintf(stderr,
                          "hipMemcpy (H2D stage after GET) failed on repeat %zu/%zu: %s\n",
                          i + 1, getRepeat, hipGetErrorString(cpyErr));
            }
          }
#endif
          ms = ElapsedMs(getStart, Clock::now());
        }
      } else {
        Aws::S3::Model::GetObjectRequest getReq;
        getReq.SetBucket(bucket);
        getReq.SetKey(key);

        auto getStart = Clock::now();
        auto getOutcome = client.GetObjectRdma(getReq, ioBuf, objSize);
        ok = getOutcome.IsSuccess();
        if (!ok) {
          ms = ElapsedMs(getStart, Clock::now());
          std::fprintf(stderr, "GetObjectRdma failed on repeat %zu/%zu: %s\n",
                      i + 1, getRepeat,
                      getOutcome.GetError().GetMessage().c_str());
        } else {
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
          if (hostStageBuf) {
            hipError_t cpyErr = hipMemcpy(devPtr, hostStageBuf, objSize, hipMemcpyHostToDevice);
            if (cpyErr != hipSuccess) {
              ok = false;
              std::fprintf(stderr,
                          "hipMemcpy (H2D stage after GetObjectRdma) failed on repeat %zu/%zu: %s\n",
                          i + 1, getRepeat, hipGetErrorString(cpyErr));
            }
          }
#endif
          ms = ElapsedMs(getStart, Clock::now());
        }
      }
      if (!ok) {
        exitCode = 1;
        break;
      }
      getTimesMs.push_back(ms);
      if (getRepeat == 1) {
        std::fprintf(stdout, "%s succeeded for %zu bytes in %.3f ms\n",
                    getLabel, objSize, ms);
        if (const char* verify = std::getenv("HIPOBJ_DEMO_VERIFY_CHECKSUM")) {
          (void)verify;
          std::fprintf(stdout, "Post-GET checksum: %016llx\n",
                      static_cast<unsigned long long>(Fnv1aHash(devPtr, objSize)));
        }
      } else {
        std::fprintf(stdout, "%s[%zu/%zu] succeeded for %zu bytes in %.3f ms\n",
                    getLabel, i + 1, getRepeat, objSize, ms);
      }
    }
    if (exitCode == 0 && getRepeat > 1) {
      PrintRepeatSummary(getLabel, getTimesMs);
    }

    if (!tcpMode) {
      hipObjError_t deregErr = client.DeregisterBuffer(ioBuf);
      if (deregErr.opError != hipObjSuccess) {
        std::fprintf(stderr, "DeregisterBuffer failed: %s\n",
                    hipObjGetErrorString(deregErr.opError));
      }
    }
#ifdef HIPOBJ_DEMO_HAS_REAL_HIP
    if (hostStageBuf) {
      (void)hipHostFree(hostStageBuf);
    }
#endif
    (void)hipFree(devPtr);
  }
  Aws::ShutdownAPI(options);
  return exitCode;
}
