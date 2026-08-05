/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* AMD port of minio-cpp GetPutRDMA: PUT + GET over hipObject RDMA */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <hip/hip_runtime.h>

#include <miniocpp/client.h>

#include "hipobj_minio/client.hpp"

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
    err = hipMemset(dev_ptr, 'A', bufsize);
    if (err != hipSuccess) {
      std::cerr << "hipMemset failed: " << err << std::endl;
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
    std::memset(bufptr, 'A', bufsize);
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

  if (gpu_enabled) {
    (void)hipMemset(dev_ptr, 'U', bufsize);
    (void)hipDeviceSynchronize();
  } else {
    std::memset(bufptr, 'U', bufsize);
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

  bool ok = true;
  for (size_t i = 0; i < bufsize; ++i) {
    if (hostptr[i] != 'A') {
      ok = false;
      break;
    }
  }
  std::cout << (ok ? "Data integrity check passed\n"
                   : "Data integrity check FAILED\n");

  free(hostptr);
  if (gpu_enabled) {
    (void)hipFree(dev_ptr);
  } else {
    free(bufptr);
  }

  return ok ? 0 : 1;
}
