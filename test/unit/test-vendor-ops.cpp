/* Copyright (c) Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include "vendor-ops.hpp"

TEST(VendorOps, ProviderNameBnxt) {
  EXPECT_STREQ(hipObj::provider_name(hipObj::Provider::BNXT), "bnxt");
}

TEST(VendorOps, ProviderNameIonic) {
  EXPECT_STREQ(hipObj::provider_name(hipObj::Provider::IONIC), "ionic");
}

TEST(VendorOps, ProviderNameUnknown) {
  EXPECT_STREQ(hipObj::provider_name(hipObj::Provider::UNKNOWN), "unknown");
}

TEST(VendorOps, FromStringBnxt) {
  EXPECT_EQ(hipObj::provider_from_string("bnxt"), hipObj::Provider::BNXT);
}

TEST(VendorOps, FromStringBnxtRe) {
  EXPECT_EQ(hipObj::provider_from_string("bnxt_re"), hipObj::Provider::BNXT);
}

TEST(VendorOps, FromStringIonic) {
  EXPECT_EQ(hipObj::provider_from_string("ionic"), hipObj::Provider::IONIC);
}

TEST(VendorOps, FromStringPensando) {
  EXPECT_EQ(hipObj::provider_from_string("pensando"), hipObj::Provider::IONIC);
}

TEST(VendorOps, FromStringUnknown) {
  EXPECT_EQ(hipObj::provider_from_string("mlx5"), hipObj::Provider::UNKNOWN);
}

TEST(VendorOps, VendorIdBroadcom) {
  EXPECT_EQ(hipObj::VENDOR_ID_BROADCOM, static_cast<uint32_t>(0x14E4));
}

TEST(VendorOps, VendorIdPensando) {
  EXPECT_EQ(hipObj::VENDOR_ID_PENSANDO, static_cast<uint32_t>(0x1DD8));
}
