/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <miniocpp/args.h>
#include <miniocpp/client.h>
#include <miniocpp/credentials.h>
#include <miniocpp/response.h>

namespace hipobj::minio {

/* How the last transfers actually went out.
 *
 * Client::PutObject and Client::GetObject fall back to ordinary HTTP in two
 * places -- when hipObjInit cannot find a NIC, and when an RDMA transfer
 * returns an error -- and the caller sees a successful response either way.
 * That is the right behaviour for a client and the wrong behaviour for a
 * test: an integration lane that is supposed to be proving the RDMA data
 * path will pass just as happily over TCP. These counters are how a caller
 * tells the two apart after the fact.
 *
 * Process-wide and monotonic. Snapshot with TransferStatsSnapshot() before
 * the transfers you care about and subtract. */
struct TransferStats {
  uint64_t rdmaPuts = 0;
  uint64_t rdmaGets = 0;
  uint64_t httpPuts = 0;
  uint64_t httpGets = 0;
  uint64_t rdmaBytes = 0;
  uint64_t httpBytes = 0;
};

/* A snapshot of the process-wide counters. */
TransferStats TransferStatsSnapshot();

/* Reset the counters to zero. */
void TransferStatsReset();

/* One line, stable and greppable, for a CI step to assert on:
 *
 *   hipobj-stats: rdma_put=1 rdma_get=1 http_put=0 http_get=0
 *                 rdma_bytes=131072 http_bytes=0
 *
 * A lane that requires RDMA asserts http_put=0 and http_get=0; a lane that
 * cannot have it (no verbs device, e.g. a plain container) asserts the
 * reverse, so the expectation is written down either way. */
std::string TransferStatsLine(const TransferStats& stats);

class Client {
public:
  Client(::minio::s3::BaseUrl base_url, ::minio::creds::Provider* provider);

  ::minio::s3::PutObjectResponse PutObject(::minio::s3::PutObjectArgs args);
  ::minio::s3::GetObjectResponse GetObject(::minio::s3::GetObjectArgs args);

  bool RdmaAvailable() const;

private:
  ::minio::s3::BaseUrl base_url_;
  ::minio::creds::Provider* provider_ = nullptr;
  ::minio::s3::Client s3_client_;
};

} // namespace hipobj::minio
