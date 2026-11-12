/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/h265_encoder_policy.h"

#include <memory>
#include <string_view>
#include <vector>

#include "test/gtest.h"
namespace webrtc::external_encoder {
namespace {
void ExpectSelection(std::string_view preference,
                     std::string_view hardware,
                     bool nvenc,
                     bool qsv,
                     bool x265,
                     int expected,
                     std::vector<int> expected_calls) {
  std::vector<int> calls;
  auto encoder = CreatePreferredH265Encoder(
      preference, hardware,
      [&] {
        calls.push_back(1);
        return nvenc ? std::make_unique<int>(1) : nullptr;
      },
      [&] {
        calls.push_back(2);
        return qsv ? std::make_unique<int>(2) : nullptr;
      },
      [&] {
        calls.push_back(3);
        return x265 ? std::make_unique<int>(3) : nullptr;
      });
  EXPECT_EQ(encoder ? *encoder : 0, expected);
  EXPECT_EQ(calls, expected_calls);
}
TEST(H265EncoderPolicyTest, AutomaticSoftwareWhenNoHardwareIsAvailable) {
  ExpectSelection("", "", false, false, true, 3, {1, 2, 3});
  ExpectSelection("", "", false, false, false, 0, {1, 2, 3});
}
TEST(H265EncoderPolicyTest, AutomaticPrefersAvailableHardware) {
  ExpectSelection("", "", true, true, true, 1, {1});
  ExpectSelection("", "", false, true, true, 2, {1, 2});
}
TEST(H265EncoderPolicyTest, ExplicitX265DoesNotProbeOrFallBackToHardware) {
  ExpectSelection("x265", "QSV", true, true, true, 3, {3});
  ExpectSelection("x265", "NVENC", true, true, false, 0, {3});
}
TEST(H265EncoderPolicyTest, ExplicitHardwareDoesNotFallBackToSoftware) {
  ExpectSelection("", "QSV", true, false, true, 0, {2});
  ExpectSelection("", "NVENC", false, true, true, 0, {1});
  ExpectSelection("", "QSV", true, true, true, 2, {2});
}

TEST(H265EncoderPolicyTest,
     AmfAutomaticAndExplicitSelectionKeepsVendorsIsolated) {
  for (const auto preference :
       {"", "auto", "AMF", "AMD", "NVENC", "QSV", "unknown"}) {
    std::vector<int> calls;
    auto encoder = CreatePreferredH265Encoder(
        "", preference,
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
        },
        [&] {
          calls.push_back(4);
          return std::make_unique<int>(4);
        });
    if (preference == std::string_view("NVENC")) {
      EXPECT_FALSE(encoder);
      EXPECT_EQ(calls, (std::vector<int>{1}));
    } else if (preference == std::string_view("QSV")) {
      EXPECT_FALSE(encoder);
      EXPECT_EQ(calls, (std::vector<int>{2}));
    } else if (preference == std::string_view("AMF") ||
               preference == std::string_view("AMD")) {
      ASSERT_TRUE(encoder);
      EXPECT_EQ(*encoder, 3);
      EXPECT_EQ(calls, (std::vector<int>{3}));
    } else if (preference == std::string_view("unknown")) {
      ASSERT_TRUE(encoder);
      EXPECT_EQ(*encoder, 4);
      EXPECT_EQ(calls, (std::vector<int>{1, 2, 4}));
    } else {
      ASSERT_TRUE(encoder);
      EXPECT_EQ(*encoder, 3);
      EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
    }
  }
}

TEST(H265EncoderPolicyTest, UnavailableAmfDoesNotBecomeAnotherCodecOrVendor) {
  for (const auto preference : {"AMF", "AMD", ""}) {
    std::vector<int> calls;
    auto encoder = CreatePreferredH265Encoder(
        "", preference,
        [&]() -> std::unique_ptr<int> {
          calls.push_back(1);
          return nullptr;
        },
        [&]() -> std::unique_ptr<int> {
          calls.push_back(2);
          return nullptr;
        },
        [&]() -> std::unique_ptr<int> {
          calls.push_back(3);
          return nullptr;
        },
        [&] {
          calls.push_back(4);
          return std::make_unique<int>(4);
        });
    if (preference == std::string_view("")) {
      ASSERT_TRUE(encoder);
      EXPECT_EQ(*encoder, 4);
      EXPECT_EQ(calls, (std::vector<int>{1, 2, 3, 4}));
    } else {
      EXPECT_FALSE(encoder);
      EXPECT_EQ(calls, (std::vector<int>{3}));
    }
  }
}

TEST(H265EncoderPolicyTest, ExplicitSoftwareSkipsAllThreeHardwareProbes) {
  int probes = 0;
  auto unavailable = [&]() -> std::unique_ptr<int> {
    ++probes;
    return nullptr;
  };
  auto encoder = CreatePreferredH265Encoder(
      "x265", "AMF", unavailable, unavailable, unavailable,
      [] { return std::make_unique<int>(4); });
  ASSERT_TRUE(encoder);
  EXPECT_EQ(*encoder, 4);
  EXPECT_EQ(probes, 0);
}

TEST(H265EncoderPolicyTest,
     RetirementFailureStopsAutomaticVendorAndSoftwareSelection) {
  for (const int failing_vendor : {1, 2, 3}) {
    bool failed = false;
    std::vector<int> calls;
    const auto create = [&](int vendor) -> std::unique_ptr<int> {
      calls.push_back(vendor);
      failed = vendor == failing_vendor;
      return nullptr;
    };
    auto encoder = CreatePreferredH265Encoder(
        "", "", [&] { return create(1); }, [&] { return create(2); },
        [&] { return create(3); }, [&] { return create(4); }, &failed);
    EXPECT_FALSE(encoder);
    EXPECT_TRUE(failed);
    EXPECT_EQ(calls.size(), static_cast<size_t>(failing_vendor));
  }
}

TEST(H265EncoderPolicyTest,
     ExplicitSoftwareIsIndependentOfPriorHardwareRetirementFailure) {
  bool failed = true;
  int hardware_calls = 0;
  int software_calls = 0;
  const auto hardware = [&]() -> std::unique_ptr<int> {
    ++hardware_calls;
    return nullptr;
  };
  const auto software = [&] {
    ++software_calls;
    return std::make_unique<int>(4);
  };
  EXPECT_FALSE(CreatePreferredH265Encoder("", "", hardware, hardware, hardware,
                                          software, &failed));
  auto encoder = CreatePreferredH265Encoder("x265", "AMF", hardware, hardware,
                                            hardware, software, &failed);
  ASSERT_TRUE(encoder);
  EXPECT_EQ(*encoder, 4);
  EXPECT_EQ(hardware_calls, 0);
  EXPECT_EQ(software_calls, 1);
}

TEST(H265EncoderPolicyTest,
     SoftwareCreatorCannotHideConcurrentRetirementFailure) {
  bool failed = false;
  const auto hardware = []() -> std::unique_ptr<int> { return nullptr; };
  const auto software = [&] {
    failed = true;
    return std::make_unique<int>(4);
  };
  EXPECT_FALSE(CreatePreferredH265Encoder("", "", hardware, hardware, hardware,
                                          software, &failed));
  failed = false;
  EXPECT_FALSE(CreatePreferredH265Encoder("", "", hardware, hardware, software,
                                          &failed));
}
}  // namespace
}  // namespace webrtc::external_encoder
