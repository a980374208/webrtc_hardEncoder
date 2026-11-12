/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#include "modules/video_coding/codecs/external/backend_video_encoder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

using external_encoder::Status;

struct BackendState {
  external_encoder::Config config;
  external_encoder::Rates rates;
  Status open_status = Status::kOk;
  Status rates_status = Status::kOk;
  Status encode_status = Status::kOk;
  int opens = 0;
  int closes = 0;
  int encodes = 0;
  bool open = false;
  bool force_key = false;
  bool wrong_frame_id = false;
  bool empty_output = false;
  bool refuse_key = false;
  uint8_t output_profile = 0x42;
  uint8_t output_level = 31;
  uint8_t first_y = 0;
  uint8_t first_u = 0;
  uint8_t first_v = 0;
};

class FakeBackend final : public external_encoder::VideoEncoderBackend {
 public:
  explicit FakeBackend(std::shared_ptr<BackendState> state)
      : state_(std::move(state)) {}

  external_encoder::Info GetInfo() const override {
    return {"Fake hardware backend", true};
  }
  Status Open(const external_encoder::Config& config) override {
    ++state_->opens;
    state_->config = config;
    state_->open = state_->open_status == Status::kOk;
    return state_->open_status;
  }
  Status SetRates(const external_encoder::Rates& rates) override {
    state_->rates = rates;
    return state_->rates_status;
  }
  Status Encode(const external_encoder::Frame& frame,
                bool force_key,
                external_encoder::Packet* packet) override {
    ++state_->encodes;
    state_->force_key = force_key;
    EXPECT_TRUE(state_->open);
    EXPECT_EQ(frame.width, state_->config.width);
    EXPECT_EQ(frame.height, state_->config.height);
    EXPECT_GE(frame.stride_y, frame.width);
    EXPECT_GE(frame.stride_uv, frame.width);
    state_->first_y = frame.y[0];
    state_->first_u = frame.uv[0];
    state_->first_v = frame.uv[1];
    if (state_->encode_status != Status::kOk)
      return state_->encode_status;
    packet->frame_id = frame.id + (state_->wrong_frame_id ? 1 : 0);
    packet->key_frame = force_key && !state_->refuse_key;
    packet->qp = 24;
    if (!state_->empty_output) {
      // The adapter validates framing/profile, not decoded picture content.
      // Real encode/RTP/decode content is covered by nvenc_h264_smoke.
      if (packet->key_frame) {
        packet->bytes = {0,    0,
                         0,    1,
                         0x67, state_->output_profile,
                         0,    state_->output_level,
                         0xf4, 0x05,
                         0x03, 0xc7,
                         0xc0, 0,
                         0,    0,
                         1,    0x68,
                         0xce, 0x3c,
                         0x80, 0,
                         0,    0,
                         1,    0x65,
                         0xff};
      } else {
        packet->bytes = {0, 0, 0, 1, 0x41, 0xff};
      }
    }
    return Status::kOk;
  }
  void Close() override {
    ++state_->closes;
    state_->open = false;
  }
  std::string_view LastError() const override { return "injected failure"; }

 private:
  const std::shared_ptr<BackendState> state_;
};

class Collector final : public EncodedImageCallback {
 public:
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* info) override {
    images.push_back(image);
    EXPECT_NE(info, nullptr);
    if (info)
      infos.push_back(*info);
    if (on_encoded)
      on_encoded();
    return Result(Result::OK, image.RtpTimestamp());
  }
  std::vector<EncodedImage> images;
  std::vector<CodecSpecificInfo> infos;
  std::function<void()> on_encoded;
};

VideoCodec CodecSettings() {
  VideoCodec codec;
  codec.codecType = kVideoCodecH264;
  codec.width = 32;
  codec.height = 16;
  codec.startBitrate = 300;
  codec.minBitrate = 30;
  codec.maxBitrate = 1000;
  codec.maxFramerate = 30;
  codec.active = true;
  codec.numberOfSimulcastStreams = 0;
  *codec.H264() = VideoEncoder::GetDefaultH264Settings();
  codec.H264()->numberOfTemporalLayers = 1;
  return codec;
}

VideoFrame InputFrame(uint32_t timestamp = 9000) {
  auto buffer = I420Buffer::Create(32, 16);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 32; ++x)
      buffer->MutableDataY()[y * buffer->StrideY() + x] = 80;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 16; ++x) {
      buffer->MutableDataU()[y * buffer->StrideU() + x] = 90;
      buffer->MutableDataV()[y * buffer->StrideV() + x] = 180;
    }
  }
  return VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_rtp_timestamp(timestamp)
      .set_timestamp_us(123000)
      .set_ntp_time_ms(456)
      .set_rotation(kVideoRotation_90)
      .build();
}

class BackendVideoEncoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    encoder_ = CreateBackendVideoEncoder(
        CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel3_1,
                         "1"),
        std::make_unique<FakeBackend>(state_));
    ASSERT_NE(encoder_, nullptr);
    ASSERT_EQ(encoder_->RegisterEncodeCompleteCallback(&collector_),
              WEBRTC_VIDEO_CODEC_OK);
  }
  void TearDown() override {
    if (encoder_)
      encoder_->Release();
  }
  int Init() {
    return encoder_->InitEncode(
        &codec_,
        VideoEncoder::Settings(VideoEncoder::Capabilities(false), 1, 1200));
  }
  void Rates(uint32_t bitrate_bps, double fps) {
    VideoBitrateAllocation allocation;
    ASSERT_TRUE(allocation.SetBitrate(0, 0, bitrate_bps));
    encoder_->SetRates(VideoEncoder::RateControlParameters(allocation, fps));
  }
  std::shared_ptr<BackendState> state_ = std::make_shared<BackendState>();
  VideoCodec codec_ = CodecSettings();
  Collector collector_;
  std::unique_ptr<VideoEncoder> encoder_;
};

TEST_F(BackendVideoEncoderTest, ConvertsPlanesAndPreservesFrameMetadata) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state_->config.bitrate_bps, 300000u);
  EXPECT_EQ(state_->config.framerate_fps, 30u);
  const VideoFrame input = InputFrame();
  ASSERT_EQ(encoder_->Encode(input, nullptr), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(collector_.images.size(), 1u);
  ASSERT_EQ(collector_.infos.size(), 1u);
  const auto& image = collector_.images[0];
  EXPECT_EQ(image.RtpTimestamp(), input.rtp_timestamp());
  EXPECT_EQ(image.capture_time_ms_, 123);
  EXPECT_EQ(image.NtpTimeMs(), 456);
  EXPECT_EQ(image.rotation(), kVideoRotation_90);
  EXPECT_EQ(image._encodedWidth, 32u);
  EXPECT_EQ(image._encodedHeight, 16u);
  EXPECT_EQ(image._frameType, VideoFrameType::kVideoFrameKey);
  EXPECT_EQ(collector_.infos[0].codecType, kVideoCodecH264);
  EXPECT_TRUE(collector_.infos[0].codecSpecific.H264.idr_frame);
  EXPECT_EQ(collector_.infos[0].codecSpecific.H264.packetization_mode,
            H264PacketizationMode::NonInterleaved);
  EXPECT_EQ(state_->first_y, 80);
  EXPECT_EQ(state_->first_u, 90);
  EXPECT_EQ(state_->first_v, 180);
  const std::vector<uint8_t> first_bytes(image.begin(), image.end());
  ASSERT_EQ(encoder_->Encode(InputFrame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(std::vector<uint8_t>(collector_.images[0].begin(),
                                 collector_.images[0].end()),
            first_bytes);
}

TEST_F(BackendVideoEncoderTest, ForcedKeyframeRequiresRealIdr) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(InputFrame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  const std::vector<VideoFrameType> key = {VideoFrameType::kVideoFrameKey};
  ASSERT_EQ(encoder_->Encode(InputFrame(12000), &key), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(state_->force_key);
  state_->refuse_key = true;
  EXPECT_EQ(encoder_->Encode(InputFrame(15000), &key),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(collector_.images.size(), 2u);
}

TEST_F(BackendVideoEncoderTest,
       PauseDoesNotSubmitFramesAndResumeStartsWithIdr) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(InputFrame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  const int encoded_before_pause = state_->encodes;
  Rates(0, 30);
  EXPECT_EQ(encoder_->Encode(InputFrame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state_->encodes, encoded_before_pause);
  EXPECT_EQ(collector_.images.size(), 1u);
  Rates(150000, 15);
  EXPECT_EQ(state_->rates.bitrate_bps, 150000u);
  EXPECT_EQ(state_->rates.framerate_fps, 15u);
  ASSERT_EQ(encoder_->Encode(InputFrame(18000), nullptr),
            WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(state_->force_key);
  ASSERT_EQ(collector_.images.size(), 2u);
  EXPECT_EQ(collector_.images.back()._frameType,
            VideoFrameType::kVideoFrameKey);
}

TEST_F(BackendVideoEncoderTest, MissingFramerateUsesInitEncodeValue) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  Rates(200000, 0);
  EXPECT_EQ(state_->rates.framerate_fps, 30u);
  Rates(150000, 15);
  Rates(100000, -1);
  EXPECT_EQ(state_->rates.framerate_fps, 30u);
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
}

TEST_F(BackendVideoEncoderTest, RateFailureIsReportedBeforeFurtherSubmission) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->rates_status = Status::kError;
  Rates(150000, 15);
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(state_->encodes, 0);
  EXPECT_TRUE(collector_.images.empty());
}

TEST_F(BackendVideoEncoderTest, RuntimeFailuresRequestSoftwareFallback) {
  for (Status status :
       {Status::kUnavailable, Status::kError, Status::kTimeout}) {
    ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
    state_->encode_status = status;
    EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
              WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
    EXPECT_TRUE(collector_.images.empty());
    encoder_->Release();
  }
}

TEST_F(BackendVideoEncoderTest, OpenFailuresDoNotSubmitFrames) {
  for (Status status :
       {Status::kUnavailable, Status::kUnsupported, Status::kError}) {
    state_->open_status = status;
    EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
    EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
              WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
    EXPECT_EQ(state_->encodes, 0);
    EXPECT_FALSE(state_->open);
    EXPECT_TRUE(collector_.images.empty());
    encoder_->Release();
  }
}

TEST_F(BackendVideoEncoderTest,
       ProcessRetirementFailureDuringOpenStaysVisibleOnRelease) {
  state_->open_status = Status::kRetirementFailed;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  const int opens = state_->opens;
  state_->open_status = Status::kOk;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(state_->opens, opens);
  EXPECT_TRUE(collector_.images.empty());
}

TEST_F(BackendVideoEncoderTest,
       ProcessRetirementFailureDuringRateUpdatePreventsReusableRelease) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->rates_status = Status::kRetirementFailed;
  Rates(150000, 15);
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(state_->encodes, 0);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ERROR);
}

TEST_F(BackendVideoEncoderTest,
       ProcessRetirementFailureDuringEncodePreventsReusableRelease) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->encode_status = Status::kRetirementFailed;
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_TRUE(collector_.images.empty());
}

TEST_F(BackendVideoEncoderTest, MissingOrMismatchedOutputIsNotDelivered) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->wrong_frame_id = true;
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_TRUE(collector_.images.empty());
  state_->wrong_frame_id = false;
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->empty_output = true;
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_TRUE(collector_.images.empty());
}

TEST_F(BackendVideoEncoderTest, BackendCanDropWithoutDeliveringDelayedFrame) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->encode_status = Status::kNoOutput;
  EXPECT_GE(encoder_->Encode(InputFrame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(collector_.images.empty());
  state_->encode_status = Status::kOk;
  ASSERT_EQ(encoder_->Encode(InputFrame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(collector_.images.size(), 1u);
  EXPECT_EQ(collector_.images[0].RtpTimestamp(), 12000u);
}

TEST_F(BackendVideoEncoderTest, ReleaseRetiresSessionAndCanReinitialize) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(InputFrame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_FALSE(state_->open);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder_->Encode(InputFrame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_UNINITIALIZED);
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(InputFrame(15000), nullptr),
            WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(collector_.images.size(), 2u);
  EXPECT_EQ(collector_.images.back().RtpTimestamp(), 15000u);
  EXPECT_TRUE(state_->force_key);
}

TEST_F(BackendVideoEncoderTest, CallbackMaySynchronouslyReleaseEncoder) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  collector_.on_encoded = [this] {
    EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  };
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_FALSE(state_->open);
  EXPECT_EQ(encoder_->Encode(InputFrame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_UNINITIALIZED);
}

TEST_F(BackendVideoEncoderTest, RejectsUnsupportedTemporalLayerConfiguration) {
  codec_.numberOfSimulcastStreams = 1;
  codec_.simulcastStream[0].width = codec_.width;
  codec_.simulcastStream[0].height = codec_.height;
  codec_.simulcastStream[0].numberOfTemporalLayers = 2;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(state_->opens, 0);
}

TEST(BackendVideoEncoderFormatTest, RejectsUnsupportedProfilesAndMode) {
  for (auto profile : {H264Profile::kProfileConstrainedBaseline,
                       H264Profile::kProfileMain, H264Profile::kProfileHigh}) {
    EXPECT_FALSE(IsNvencH264FormatSupported(
        CreateH264Format(profile, H264Level::kLevel3_1, "1")));
  }
  EXPECT_FALSE(IsNvencH264FormatSupported(CreateH264Format(
      H264Profile::kProfileBaseline, H264Level::kLevel3_1, "0")));
}

TEST(BackendVideoEncoderFormatTest,
     PreservesNegotiatedLevelAndRejectsHigherSps) {
  auto state = std::make_shared<BackendState>();
  auto encoder = CreateBackendVideoEncoder(
      CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel2, "1"),
      std::make_unique<FakeBackend>(state));
  ASSERT_NE(encoder, nullptr);
  Collector collector;
  encoder->RegisterEncodeCompleteCallback(&collector);
  auto codec = CodecSettings();
  ASSERT_EQ(encoder->InitEncode(
                &codec, VideoEncoder::Settings(
                            VideoEncoder::Capabilities(false), 1, 1200)),
            WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state->config.h264_level, 20u);
  state->output_level = 31;
  EXPECT_EQ(encoder->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_TRUE(collector.images.empty());
  encoder->Release();
}

TEST_F(BackendVideoEncoderTest, RejectsUnexpectedSpsProfile) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->output_profile = 0x64;
  EXPECT_EQ(encoder_->Encode(InputFrame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_TRUE(collector_.images.empty());
}

}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
