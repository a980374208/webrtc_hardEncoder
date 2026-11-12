/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "modules/video_coding/codecs/external/qsv_video_encoder_backend.h"
#include "test/gtest.h"

namespace webrtc::external_encoder {
namespace {

// These tests deliberately submit only invalid configurations. They validate
// rejection before adapter discovery and require no Intel device or runtime.
// They provide no evidence of hardware encoding or valid SDK configuration.
Config ConfigFor(Codec codec) {
  Config config;
  config.codec = codec;
  config.width = 320;
  config.height = 240;
  config.bitrate_bps = 300000;
  config.framerate_fps = 30;
  return config;
}

std::unique_ptr<VideoEncoderBackend> BackendFor(Codec codec) {
  return codec == Codec::kH264 ? CreateQsvH264Backend()
                               : CreateQsvH265Backend();
}

void ExpectInvalid(VideoEncoderBackend& backend, const Config& config) {
  ASSERT_EQ(backend.Open(config), Status::kInvalidArgument);
  EXPECT_NE(backend.LastError().find("invalid"), std::string_view::npos);
}

constexpr std::array<Codec, 2> kCodecs = {Codec::kH264, Codec::kH265};

TEST(QsvBackendConfigTest, RejectsCodecMismatchBeforeAdapterDiscovery) {
  for (const auto codec : kCodecs) {
    SCOPED_TRACE(static_cast<int>(codec));
    auto backend = BackendFor(codec);
    auto config =
        ConfigFor(codec == Codec::kH264 ? Codec::kH265 : Codec::kH264);
    ExpectInvalid(*backend, config);
  }
}

TEST(QsvBackendConfigTest, RejectsUnknownOrSdkEncodedHevcLevels) {
  auto backend = CreateQsvH265Backend();
  auto config = ConfigFor(Codec::kH265);
  // In particular, SDK values 10/20/31 must not be mistaken for RTP level_idc.
  constexpr std::array<uint8_t, 9> kInvalidLevels = {0,  1,  10,  20, 31,
                                                     92, 94, 187, 255};
  for (const uint8_t level : kInvalidLevels) {
    SCOPED_TRACE(static_cast<int>(level));
    config.h265_level = level;
    ExpectInvalid(*backend, config);
  }
}

TEST(QsvBackendConfigTest, RejectsInvalidNv12DimensionsForBothCodecs) {
  for (const auto codec : kCodecs) {
    SCOPED_TRACE(static_cast<int>(codec));
    auto backend = BackendFor(codec);
    for (const int dimension : {0, -2, 319, 8194}) {
      SCOPED_TRACE(dimension);
      auto config = ConfigFor(codec);
      config.width = dimension;
      ExpectInvalid(*backend, config);
      config = ConfigFor(codec);
      config.height = dimension;
      ExpectInvalid(*backend, config);
    }
  }
}

TEST(QsvBackendConfigTest, RejectsRatesOutsideSdkFieldAndFpsBounds) {
  for (const auto codec : kCodecs) {
    SCOPED_TRACE(static_cast<int>(codec));
    auto backend = BackendFor(codec);
    for (const uint32_t bitrate : {0u, 65535001u}) {
      SCOPED_TRACE(bitrate);
      auto config = ConfigFor(codec);
      config.bitrate_bps = bitrate;
      ExpectInvalid(*backend, config);
    }
    for (const uint32_t fps : {0u, 241u}) {
      SCOPED_TRACE(fps);
      auto config = ConfigFor(codec);
      config.framerate_fps = fps;
      ExpectInvalid(*backend, config);
    }
  }
}

TEST(QsvBackendConfigTest, RejectionAndRepeatedCloseLeaveNoUsableSession) {
  for (const auto codec : kCodecs) {
    SCOPED_TRACE(static_cast<int>(codec));
    auto backend = BackendFor(codec);
    auto config = ConfigFor(codec);
    config.width = 0;
    ExpectInvalid(*backend, config);
    EXPECT_EQ(backend->SetRates({300000, 30}), Status::kError);
    EXPECT_NE(backend->LastError().find("not initialized"),
              std::string_view::npos);
    backend->Close();
    backend->Close();
    Packet packet;
    EXPECT_EQ(backend->Encode(Frame{}, true, &packet), Status::kError);
    EXPECT_TRUE(packet.bytes.empty());
    EXPECT_NE(backend->LastError().find("no idle surface"),
              std::string_view::npos);
    // Open must clear a prior closed-session error before validating the retry.
    config = ConfigFor(codec);
    config.framerate_fps = 0;
    ExpectInvalid(*backend, config);
    EXPECT_EQ(backend->LastError().find("no idle surface"),
              std::string_view::npos);
    backend->Close();
  }
}

}  // namespace
}  // namespace webrtc::external_encoder
