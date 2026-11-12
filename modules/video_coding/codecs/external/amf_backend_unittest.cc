/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include <cstdint>
#include <limits>
#include <string>

#include "modules/video_coding/codecs/external/amf_hevc_level_limits.h"
#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"
#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

Config TestConfig() {
  Config config;
  config.codec = Codec::kH264;
  config.width = 640;
  config.height = 360;
  config.bitrate_bps = 600000;
  config.framerate_fps = 30;
  return config;
}

TEST(AmfBackendTest, RejectsNullCapabilityOutput) {
  std::string reason;
  EXPECT_EQ(QueryAmfH264Capabilities(nullptr, &reason),
            Status::kInvalidArgument);
  EXPECT_FALSE(reason.empty());
}

TEST(AmfBackendTest, RejectsInvalidConfigurationsBeforeLoadingDriver) {
  auto backend = CreateAmfH264Backend();
  auto config = TestConfig();
  config.codec = Codec::kH265;
  EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  for (const int value : {-2, 0, 14, 159, 8194}) {
    SCOPED_TRACE(value);
    config = TestConfig();
    config.width = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
    config = TestConfig();
    config.height = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint8_t level : {0, 1, 9, 14, 23, 33, 43, 53, 63, 255}) {
    SCOPED_TRACE(static_cast<int>(level));
    config = TestConfig();
    config.h264_level = level;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint32_t value : {0u, 65535001u}) {
    config = TestConfig();
    config.bitrate_bps = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint32_t value : {0u, 241u}) {
    config = TestConfig();
    config.framerate_fps = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
}

TEST(AmfBackendTest, ClosedSessionRejectsEncodingAndClearsCallerPacket) {
  auto backend = CreateAmfH264Backend();
  Packet packet;
  packet.bytes = {1, 2, 3};
  packet.frame_id = 100;
  packet.key_frame = true;
  backend->Close();
  EXPECT_EQ(backend->LastCloseStatus(), Status::kOk);
  backend->Close();
  EXPECT_EQ(backend->LastCloseStatus(), Status::kOk);
  EXPECT_EQ(backend->Encode(Frame{}, true, &packet), Status::kError);
  EXPECT_TRUE(packet.bytes.empty());
  EXPECT_EQ(packet.frame_id, 0u);
  EXPECT_FALSE(packet.key_frame);
  EXPECT_EQ(backend->SetRates({600000, 30}), Status::kError);
}

TEST(AmfBackendTest, InvalidProbeLevelIsRejectedBeforeHardwareAccess) {
  std::string reason;
  EXPECT_EQ(ProbeAmfH264Backend(&reason, 33), Status::kInvalidArgument);
  EXPECT_FALSE(reason.empty());
}

#if defined(RTC_ENABLE_H265)
TEST(AmfHevcLevelTest, AcceptsActualCodedPaddingFor720And1080) {
  EXPECT_EQ(AmfHevcLevelViolation(93, 1280, 720, 30, 3000000), nullptr);
  EXPECT_EQ(AmfHevcLevelViolation(93, 1280, 736, 30, 3000000), nullptr);
  EXPECT_EQ(AmfHevcLevelViolation(120, 1920, 1088, 30, 3000000), nullptr);
  EXPECT_EQ(AmfHevcLevelViolation(30, 176, 144, 15, 64000), nullptr);
}

TEST(AmfHevcLevelTest, RejectsPictureRateAndMainTierBitrateViolations) {
  EXPECT_STREQ(AmfHevcLevelViolation(93, 1920, 1088, 30, 3000000),
               "HEVC coded picture exceeds MaxLumaPs");
  EXPECT_STREQ(AmfHevcLevelViolation(93, 1280, 720, 240, 3000000),
               "HEVC coded picture/framerate exceeds MaxLumaSr");
  EXPECT_STREQ(AmfHevcLevelViolation(93, 1280, 720, 30, 10000001),
               "HEVC configured bitrate exceeds Main-tier MaxBR");
  EXPECT_EQ(AmfHevcLevelViolation(120, 2048, 1088, 30, 12000000), nullptr);
  EXPECT_STREQ(AmfHevcLevelViolation(120, 2048, 1088, 31, 12000000),
               "HEVC coded picture/framerate exceeds MaxLumaSr");
  EXPECT_STREQ(AmfHevcLevelViolation(120, 2048, 1096, 30, 12000000),
               "HEVC coded picture exceeds MaxLumaPs");
  EXPECT_STREQ(AmfHevcLevelViolation(120, 2048, 1088, 30, 12000001),
               "HEVC configured bitrate exceeds Main-tier MaxBR");
}

TEST(AmfHevcLevelTest, CropCannotHideCodedPaddingOrExcessiveAspectRatio) {
  // The visible 720p picture reaches the exact sample-rate boundary at 36
  // FPS. Cropping a padded coded height back to 720 cannot keep that claim.
  EXPECT_EQ(AmfHevcLevelViolation(93, 1280, 720, 36, 3000000), nullptr);
  EXPECT_STREQ(AmfHevcLevelViolation(93, 1280, 736, 36, 3000000),
               "HEVC coded picture/framerate exceeds MaxLumaSr");
  EXPECT_EQ(AmfHevcLevelViolation(30, 536, 64, 15, 64000), nullptr);
  EXPECT_STREQ(AmfHevcLevelViolation(30, 544, 64, 15, 64000),
               "HEVC coded dimension exceeds sqrt(8*MaxLumaPs)");
}

TEST(AmfHevcLevelTest, RejectsInvalidAndOverflowSizedInputs) {
  EXPECT_NE(AmfHevcLevelViolation(94, 640, 360, 30, 600000), nullptr);
  EXPECT_NE(AmfHevcLevelViolation(93, 0, 360, 30, 600000), nullptr);
  EXPECT_NE(AmfHevcLevelViolation(93, 640, 0, 30, 600000), nullptr);
  EXPECT_NE(AmfHevcLevelViolation(93, 640, 360, 0, 600000), nullptr);
  EXPECT_NE(AmfHevcLevelViolation(93, 640, 360, 30, 0), nullptr);
  constexpr uint32_t maximum = std::numeric_limits<uint32_t>::max();
  EXPECT_NE(AmfHevcLevelViolation(186, maximum, maximum, maximum, maximum),
            nullptr);
  EXPECT_NE(AmfHevcLevelViolation(186, 1920, 1088, maximum, 3000000), nullptr);
}

TEST(AmfBackendTest, H265RejectsImpossibleLevelBeforeLoadingDriver) {
  auto backend = CreateAmfH265Backend();
  auto config = TestConfig();
  config.codec = Codec::kH265;
  config.width = 1920;
  config.height = 1080;
  config.h265_level = 93;
  EXPECT_EQ(backend->Open(config), Status::kUnsupported);
  EXPECT_EQ(backend->LastError(), "HEVC coded picture exceeds MaxLumaPs");
  config.width = 1280;
  config.height = 720;
  config.framerate_fps = 240;
  EXPECT_EQ(backend->Open(config), Status::kUnsupported);
  EXPECT_EQ(backend->LastError(),
            "HEVC coded picture/framerate exceeds MaxLumaSr");
  config.framerate_fps = 30;
  config.bitrate_bps = 10000001;
  EXPECT_EQ(backend->Open(config), Status::kUnsupported);
  EXPECT_EQ(backend->LastError(),
            "HEVC configured bitrate exceeds Main-tier MaxBR");
  EXPECT_EQ(backend->LastCloseStatus(), Status::kOk);
}

TEST(AmfBackendTest, RejectsNullH265CapabilityOutput) {
  std::string reason;
  EXPECT_EQ(QueryAmfH265Capabilities(nullptr, &reason),
            Status::kInvalidArgument);
  EXPECT_FALSE(reason.empty());
}

TEST(AmfBackendTest, H265RejectsInvalidConfigurationsBeforeLoadingDriver) {
  auto backend = CreateAmfH265Backend();
  auto config = TestConfig();
  EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  for (const uint8_t level : {0, 1, 29, 31, 61, 94, 119, 124, 157, 187, 255}) {
    SCOPED_TRACE(static_cast<int>(level));
    config = TestConfig();
    config.codec = Codec::kH265;
    config.h265_level = level;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const int value : {-2, 0, 14, 159, 8194}) {
    config = TestConfig();
    config.codec = Codec::kH265;
    config.width = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
    config = TestConfig();
    config.codec = Codec::kH265;
    config.height = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint32_t value : {0u, 65535001u}) {
    config = TestConfig();
    config.codec = Codec::kH265;
    config.bitrate_bps = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
  for (const uint32_t value : {0u, 241u}) {
    config = TestConfig();
    config.codec = Codec::kH265;
    config.framerate_fps = value;
    EXPECT_EQ(backend->Open(config), Status::kInvalidArgument);
  }
}

TEST(AmfBackendTest, ClosedH265SessionAndFailedOpenNeverReturnStalePacket) {
  auto backend = CreateAmfH265Backend();
  EXPECT_EQ(backend->GetInfo().implementation_name, "AMF H265 / D3D11");
  EXPECT_TRUE(backend->GetInfo().is_hardware_accelerated);
  EXPECT_EQ(backend->Open(TestConfig()), Status::kInvalidArgument);
  Packet packet;
  packet.bytes = {1, 2, 3};
  packet.frame_id = 100;
  packet.key_frame = true;
  backend->Close();
  EXPECT_EQ(backend->LastCloseStatus(), Status::kOk);
  backend->Close();
  EXPECT_EQ(backend->LastCloseStatus(), Status::kOk);
  EXPECT_EQ(backend->Encode(Frame{}, true, &packet), Status::kError);
  EXPECT_TRUE(packet.bytes.empty());
  EXPECT_EQ(packet.frame_id, 0u);
  EXPECT_FALSE(packet.key_frame);
  EXPECT_EQ(backend->Encode(Frame{}, true, nullptr), Status::kError);
  EXPECT_EQ(backend->SetRates({600000, 30}), Status::kError);
}

TEST(AmfBackendTest, InvalidH265ProbeLevelIsRejectedBeforeHardwareAccess) {
  std::string reason;
  EXPECT_EQ(ProbeAmfH265Backend(&reason, 94), Status::kInvalidArgument);
  EXPECT_FALSE(reason.empty());
}
#endif

}  // namespace
}  // namespace webrtc::external_encoder
