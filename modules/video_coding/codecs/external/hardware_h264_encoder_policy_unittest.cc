/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/hardware_h264_encoder_policy.h"

#include <memory>
#include <string_view>
#include <vector>

#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

void CheckSelection(std::string_view preference,
                    bool nvenc_available,
                    bool qsv_available,
                    int expected,
                    int nvenc_calls,
                    int qsv_calls) {
  int called_nvenc = 0;
  int called_qsv = 0;
  auto selected = CreatePreferredHardwareEncoder(
      preference,
      [&] {
        ++called_nvenc;
        return nvenc_available ? std::make_unique<int>(1) : nullptr;
      },
      [&] {
        ++called_qsv;
        return qsv_available ? std::make_unique<int>(2) : nullptr;
      });
  EXPECT_EQ(selected ? *selected : 0, expected);
  EXPECT_EQ(called_nvenc, nvenc_calls);
  EXPECT_EQ(called_qsv, qsv_calls);
}

TEST(HardwareH264EncoderPolicyTest, DefaultSelectionAndUnavailableHardware) {
  CheckSelection("", true, true, 1, 1, 0);
  CheckSelection("", true, false, 1, 1, 0);
  CheckSelection("", false, true, 2, 1, 1);
  CheckSelection("", false, false, 0, 1, 1);
}

TEST(HardwareH264EncoderPolicyTest, ExplicitQsvNeverAttemptsNvenc) {
  CheckSelection("QSV", true, true, 2, 0, 1);
  CheckSelection("QSV", true, false, 0, 0, 1);
}

TEST(HardwareH264EncoderPolicyTest, ExplicitNvencNeverAttemptsQsv) {
  CheckSelection("NVENC", true, true, 1, 1, 0);
  CheckSelection("NVENC", false, true, 0, 1, 0);
}

TEST(HardwareH264EncoderPolicyTest, UnknownPreferenceUsesDefaultSelection) {
  CheckSelection("unknown", true, true, 1, 1, 0);
  CheckSelection("unknown", false, true, 2, 1, 1);
}

TEST(HardwareH264EncoderPolicyTest, ExplicitAmfIsolatedEvenWhenUnavailable) {
  for (std::string_view preference : {"AMF", "AMD"}) {
    int other_calls = 0;
    int amf_calls = 0;
    auto selected = CreatePreferredH264HardwareEncoder(
        preference,
        [&] {
          ++other_calls;
          return std::make_unique<int>(1);
        },
        [&] {
          ++other_calls;
          return std::make_unique<int>(2);
        },
        [&]() -> std::unique_ptr<int> {
          ++amf_calls;
          return nullptr;
        });
    EXPECT_FALSE(selected);
    EXPECT_EQ(other_calls, 0);
    EXPECT_EQ(amf_calls, 1);
  }
}

TEST(HardwareH264EncoderPolicyTest, AutomaticAmfFollowsUnavailableVendors) {
  std::vector<int> calls;
  auto selected = CreatePreferredH264HardwareEncoder(
      "auto",
      [&]() -> std::unique_ptr<int> {
        calls.push_back(1);
        return nullptr;
      },
      [&]() -> std::unique_ptr<int> {
        calls.push_back(2);
        return nullptr;
      },
      [&] {
        calls.push_back(3);
        return std::make_unique<int>(3);
      });
  ASSERT_TRUE(selected);
  EXPECT_EQ(*selected, 3);
  EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
}

TEST(HardwareH264EncoderPolicyTest, UnknownPreferenceDoesNotExpandToAmf) {
  int amf_calls = 0;
  auto selected = CreatePreferredH264HardwareEncoder(
      "unknown", []() -> std::unique_ptr<int> { return nullptr; },
      []() -> std::unique_ptr<int> { return nullptr; },
      [&] {
        ++amf_calls;
        return std::make_unique<int>(3);
      });
  EXPECT_FALSE(selected);
  EXPECT_EQ(amf_calls, 0);
}

TEST(HardwareH264EncoderPolicyTest,
     RetirementFailureStopsEveryAutomaticVendorStage) {
  for (const int failing_vendor : {1, 2, 3}) {
    bool failed = false;
    std::vector<int> calls;
    const auto create = [&](int vendor) -> std::unique_ptr<int> {
      calls.push_back(vendor);
      failed = vendor == failing_vendor;
      return nullptr;
    };
    auto encoder = CreatePreferredH264HardwareEncoder(
        "auto", [&] { return create(1); }, [&] { return create(2); },
        [&] { return create(3); }, &failed);
    EXPECT_FALSE(encoder);
    EXPECT_TRUE(failed);
    EXPECT_EQ(calls.size(), static_cast<size_t>(failing_vendor));
  }
}

TEST(HardwareH264EncoderPolicyTest,
     PriorRetirementFailureSkipsCreatorsAndCannotReturnTheirEncoder) {
  bool failed = true;
  int calls = 0;
  auto create = [&] {
    ++calls;
    return std::make_unique<int>(1);
  };
  EXPECT_FALSE(CreatePreferredH264HardwareEncoder("auto", create, create,
                                                  create, &failed));
  EXPECT_FALSE(
      CreatePreferredHardwareEncoder("NVENC", create, create, &failed));
  EXPECT_EQ(calls, 0);
  failed = false;
  auto inconsistent = [&] {
    ++calls;
    failed = true;
    return std::make_unique<int>(1);
  };
  EXPECT_FALSE(
      CreatePreferredHardwareEncoder("QSV", create, inconsistent, &failed));
  EXPECT_EQ(calls, 1);
}

}  // namespace
}  // namespace webrtc::external_encoder
