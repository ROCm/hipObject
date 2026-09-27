/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* AMD port of minio-cpp GetPutRDMA: PUT + GET over hipObject RDMA */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <hip/hip_runtime.h>

#include <miniocpp/client.h>
#include <unistd.h>

#include "hipobj_minio/client.h"

namespace {

/* A position-dependent pattern, not a constant fill.
 *
 * This used to memset the buffer to 'A' and check afterwards that every byte
 * was still 'A', which cannot distinguish a correct transfer from a
 * byte-swapped, misaligned, short, duplicated or scattered one -- every one
 * of those returns a buffer full of 'A'. A cheap LCG keyed on the offset
 * makes each byte depend on where it is, so any of those failures shows up
 * as a mismatch at a specific offset, which is also the first thing you want
 * to know when it does. */
uint8_t PatternByte(size_t offset, uint32_t seed) {
  uint64_t x = (static_cast<uint64_t>(offset) + 1) * 6364136223846793005ULL +
               seed;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return static_cast<uint8_t>(x & 0xff);
}

void FillPattern(char* buf, size_t size, uint32_t seed) {
  for (size_t i = 0; i < size; ++i) {
    buf[i] = static_cast<char>(PatternByte(i, seed));
  }
}

/* Returns the offset of the first mismatch, or size if the blob read back
 * matches the blob written byte for byte. */
size_t FirstMismatch(const char* buf, size_t size, uint32_t seed) {
  for (size_t i = 0; i < size; ++i) {
    if (static_cast<uint8_t>(buf[i]) != PatternByte(i, seed)) {
      return i;
    }
  }
  return size;
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::cerr << "usage: " << argv[0]
              << " <host> <access_key> <secret_key> [size_bytes] [gpu]\n";
    return 1;
  }

  const std::string host = argv[1];
  const std::string access_key = argv[2];
  const std::string secret_key = argv[3];
  size_t bufsize = 10 * 1024 * 1024UL;
  bool gpu_enabled = true;

  if (argc >= 5) {
    bufsize = static_cast<size_t>(std::atol(argv[4]));
  }
  if (argc >= 6) {
    gpu_enabled = std::string(argv[5]) == "gpu";
  }

  /* Seeded per run, and printed, so a stale object left in the store by an
   * earlier run cannot satisfy the readback. */
  const uint32_t seed = static_cast<uint32_t>(::getpid()) ^
                        static_cast<uint32_t>(std::time(nullptr));
  std::cout << "Pattern seed " << seed << "\n";

  std::vector<char> expected(bufsize);
  FillPattern(expected.data(), bufsize, seed);

  minio::s3::BaseUrl base_url(host, false, "us-east-1");
  minio::creds::StaticProvider provider(access_key, secret_key);
  hipobj::minio::Client client(base_url, &provider);

  char* bufptr = nullptr;
  void* dev_ptr = nullptr;

  if (gpu_enabled) {
    hipError_t err = hipMalloc(&dev_ptr, bufsize);
    if (err != hipSuccess) {
      std::cerr << "hipMalloc failed: " << err << std::endl;
      return 1;
    }
    err = hipMemcpy(dev_ptr, expected.data(), bufsize, hipMemcpyHostToDevice);
    if (err != hipSuccess) {
      std::cerr << "hipMemcpy H2D failed: " << err << std::endl;
      (void)hipFree(dev_ptr);
      return 1;
    }
    (void)hipDeviceSynchronize();
    bufptr = static_cast<char*>(dev_ptr);
    std::cout << "GPU buffer " << bufsize << " bytes\n";
  } else {
    int res = posix_memalign(reinterpret_cast<void**>(&bufptr), getpagesize(),
                             bufsize);
    if (res != 0 || bufptr == nullptr) {
      std::cerr << "posix_memalign failed\n";
      return 1;
    }
    std::memcpy(bufptr, expected.data(), bufsize);
    std::cout << "Host buffer " << bufsize << " bytes\n";
  }

  minio::s3::PutObjectArgs pargs;
  pargs.buf = bufptr;
  pargs.size = bufsize;
  pargs.bucket = "hipobj-test";
  pargs.object = "my-object";

  minio::s3::PutObjectResponse presp = client.PutObject(pargs);
  if (!presp) {
    std::cerr << "PUT failed: " << presp.Error().String() << std::endl;
    if (gpu_enabled) {
      (void)hipFree(dev_ptr);
    } else {
      free(bufptr);
    }
    return 1;
  }
  std::cout << "PUT ok etag=" << presp.etag << std::endl;

  /* Clobber the buffer before the GET. Without this the readback could be
   * satisfied by whatever the PUT left behind, and a GET that transferred
   * nothing at all would still verify. */
  if (gpu_enabled) {
    (void)hipMemset(dev_ptr, 0x55, bufsize);
    (void)hipDeviceSynchronize();
  } else {
    std::memset(bufptr, 0x55, bufsize);
  }

  minio::s3::GetObjectArgs gargs;
  gargs.buf = bufptr;
  gargs.size = bufsize;
  gargs.bucket = "hipobj-test";
  gargs.object = "my-object";

  minio::s3::GetObjectResponse gresp = client.GetObject(gargs);
  if (!gresp) {
    std::cerr << "GET failed: " << gresp.Error().String() << std::endl;
    if (gpu_enabled) {
      (void)hipFree(dev_ptr);
    } else {
      free(bufptr);
    }
    return 1;
  }
  std::cout << "GET ok\n";

  char* hostptr = static_cast<char*>(std::malloc(bufsize));
  if (!hostptr) {
    std::cerr << "malloc failed\n";
    if (gpu_enabled) {
      (void)hipFree(dev_ptr);
    } else {
      free(bufptr);
    }
    return 1;
  }

  if (gpu_enabled) {
    hipError_t err = hipMemcpy(hostptr, dev_ptr, bufsize,
                               hipMemcpyDeviceToHost);
    if (err != hipSuccess) {
      std::cerr << "hipMemcpy failed: " << err << std::endl;
      free(hostptr);
      (void)hipFree(dev_ptr);
      return 1;
    }
  } else {
    std::memcpy(hostptr, bufptr, bufsize);
  }

  std::ofstream out("output.bin", std::ios::binary);
  if (!out) {
    std::cerr << "failed to open output.bin\n";
    free(hostptr);
    if (gpu_enabled) {
      (void)hipFree(dev_ptr);
    } else {
      free(bufptr);
    }
    return 1;
  }
  out.write(hostptr, static_cast<std::streamsize>(bufsize));
  out.close();
  std::cout << "Wrote output.bin (" << bufsize << " bytes)\n";

  size_t bad = FirstMismatch(hostptr, bufsize, seed);
  bool ok = bad == bufsize;
  if (ok) {
    std::cout << "Data integrity check passed (" << bufsize << " bytes, seed "
              << seed << ")\n";
  } else {
    std::cout << "Data integrity check FAILED at offset " << bad
              << ": expected 0x" << std::hex
              << static_cast<int>(PatternByte(bad, seed)) << " got 0x"
              << static_cast<int>(static_cast<uint8_t>(hostptr[bad]))
              << std::dec << "\n";
  }

  /* Whether the transfers actually used RDMA. The client falls back to
   * ordinary HTTP silently when no NIC is available, so without this a lane
   * that is meant to be exercising the RDMA data path passes over TCP. */
  std::cout << hipobj::minio::TransferStatsLine(
                 hipobj::minio::TransferStatsSnapshot())
            << "\n";

  free(hostptr);
  if (gpu_enabled) {
    (void)hipFree(dev_ptr);
  } else {
    free(bufptr);
  }

  return ok ? 0 : 1;
}
