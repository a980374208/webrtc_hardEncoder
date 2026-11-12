/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

using external_encoder::Codec;
using external_encoder::Status;

// These fakes validate shared adapter/wrapper error propagation. They do not
// model SDK retirement ordering or establish physical device-lost safety.
struct RetirementState {
  Status open_result = Status::kOk;
  Status encode_result = Status::kError;
  Status close_result = Status::kOk;
  bool allocated = false;
  bool fail_close = false;
  int opens = 0;
  int closes = 0;
  int encodes = 0;
  std::string diagnostic = "injected encoding failure";
};

class RetirementBackend final : public external_encoder::VideoEncoderBackend {
 public:
  explicit RetirementBackend(std::shared_ptr<RetirementState> state)
      : state_(std::move(state)) {}
  external_encoder::Info GetInfo() const override {
    return {"Injected hardware backend", true};
  }
  Status Open(const external_encoder::Config&) override {
    ++state_->opens;
    // Even failed Open may have allocated SDK resources.
    state_->allocated = true;
    return state_->open_result;
  }
  Status SetRates(const external_encoder::Rates&) override {
    return Status::kOk;
  }
  Status Encode(const external_encoder::Frame&,
                bool,
                external_encoder::Packet*) override {
    ++state_->encodes;
    return state_->encode_result;
  }
  void Close() override {
    ++state_->closes;
    if (state_->allocated && state_->fail_close) {
      state_->close_result = Status::kError;
      state_->diagnostic = "injected encoding failure; close: SDK retirement";
    }
    state_->allocated = false;
  }
  Status LastCloseStatus() const override { return state_->close_result; }
  std::string_view LastError() const override { return state_->diagnostic; }

 private:
  const std::shared_ptr<RetirementState> state_;
};

struct SoftwareState {
  int inits = 0;
  int encodes = 0;
  int releases = 0;
};

class FallbackProbeEncoder final : public VideoEncoder {
 public:
  explicit FallbackProbeEncoder(std::shared_ptr<SoftwareState> state)
      : state_(std::move(state)) {}
  int InitEncode(const VideoCodec*, const Settings&) override {
    ++state_->inits;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback*) override {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Release() override {
    ++state_->releases;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Encode(const VideoFrame&,
                 const std::vector<VideoFrameType>*) override {
    ++state_->encodes;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  void SetRates(const RateControlParameters&) override {}
  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = "Injected same-codec software";
    info.is_hardware_accelerated = false;
    return info;
  }

 private:
  const std::shared_ptr<SoftwareState> state_;
};

class Callback final : public EncodedImageCallback {
 public:
  Result OnEncodedImage(const EncodedImage&,
                        const CodecSpecificInfo*) override {
    return Result(Result::OK);
  }
};

class VendorCloseStatusTest : public ::testing::TestWithParam<Codec> {
 protected:
  VideoCodec CodecSettings() const {
    VideoCodec codec;
    codec.codecType =
        GetParam() == Codec::kH264 ? kVideoCodecH264 : kVideoCodecH265;
    codec.width = 32;
    codec.height = 16;
    codec.startBitrate = 300;
    codec.maxFramerate = 30;
    codec.active = true;
    codec.simulcastStream[0].numberOfTemporalLayers = 1;
    if (GetParam() == Codec::kH264) {
      *codec.H264() = VideoEncoder::GetDefaultH264Settings();
      codec.H264()->numberOfTemporalLayers = 1;
    }
    return codec;
  }

  std::unique_ptr<VideoEncoder> Adapter() const {
    const auto format = GetParam() == Codec::kH264
                            ? CreateH264Format(H264Profile::kProfileBaseline,
                                               H264Level::kLevel3_1, "1")
                            : SdpVideoFormat("H265", {{"profile-id", "1"},
                                                      {"tier-flag", "0"},
                                                      {"level-id", "93"},
                                                      {"tx-mode", "SRST"}});
    return CreateBackendVideoEncoder(
        format, std::make_unique<RetirementBackend>(retirement_),
        BackendFailurePolicy::kSameCodecSoftwareFallback);
  }

  int Init(VideoEncoder& encoder) const {
    const auto codec = CodecSettings();
    return encoder.InitEncode(
        &codec,
        VideoEncoder::Settings(VideoEncoder::Capabilities(false), 2, 1200));
  }

  std::unique_ptr<VideoEncoder> Wrapper() const {
    return CreateVideoEncoderSoftwareFallbackWrapper(
        CreateEnvironment(), std::make_unique<FallbackProbeEncoder>(software_),
        Adapter(), /*prefer_temporal_support=*/false);
  }

  VideoFrame Frame() const {
    auto buffer = I420Buffer::Create(32, 16);
    buffer->InitializeData();
    return VideoFrame::Builder()
        .set_video_frame_buffer(buffer)
        .set_rtp_timestamp(9000)
        .set_timestamp_us(100000)
        .build();
  }

  const std::shared_ptr<RetirementState> retirement_ =
      std::make_shared<RetirementState>();
  const std::shared_ptr<SoftwareState> software_ =
      std::make_shared<SoftwareState>();
  Callback callback_;
};

TEST_P(VendorCloseStatusTest, ReleaseFailureStaysVisibleAndPreventsReopen) {
  auto encoder = Adapter();
  ASSERT_NE(encoder, nullptr);
  ASSERT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_OK);
  retirement_->fail_close = true;
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(retirement_->opens, 1);
  EXPECT_NE(retirement_->diagnostic.find("close: SDK retirement"),
            std::string::npos);
}

TEST_P(VendorCloseStatusTest, PriorCleanupFailurePreventsInitialOpen) {
  retirement_->close_result = Status::kError;
  auto encoder = Adapter();
  ASSERT_NE(encoder, nullptr);
  EXPECT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(retirement_->opens, 0);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
}

TEST_P(VendorCloseStatusTest, InitializationFailureCannotHideFailedRetirement) {
  retirement_->open_result = Status::kError;
  retirement_->fail_close = true;
  auto encoder = Wrapper();
  ASSERT_NE(encoder, nullptr);
  EXPECT_NE(Init(*encoder), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(software_->inits, 0);
  EXPECT_EQ(software_->releases, 0);
  EXPECT_EQ(software_->encodes, 0);
  EXPECT_TRUE(encoder->GetEncoderInfo().is_hardware_accelerated);
  EXPECT_EQ(retirement_->close_result, Status::kError);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_NE(Init(*encoder), WEBRTC_VIDEO_CODEC_OK);
}

TEST_P(VendorCloseStatusTest, EncodeFailureCannotHideFailedRetirement) {
  auto encoder = Wrapper();
  ASSERT_NE(encoder, nullptr);
  ASSERT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_OK);
  encoder->RegisterEncodeCompleteCallback(&callback_);
  retirement_->fail_close = true;
  EXPECT_NE(encoder->Encode(Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(software_->inits, 1);
  EXPECT_EQ(software_->releases, 1);
  EXPECT_EQ(software_->encodes, 0);
  EXPECT_TRUE(encoder->GetEncoderInfo().is_hardware_accelerated);
  EXPECT_EQ(encoder->Encode(Frame(), nullptr), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(software_->inits, 1);
  EXPECT_EQ(software_->encodes, 0);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
}

TEST_P(VendorCloseStatusTest, SuccessfulRetirementAllowsSameCodecFallback) {
  auto encoder = Wrapper();
  ASSERT_NE(encoder, nullptr);
  ASSERT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_OK);
  encoder->RegisterEncodeCompleteCallback(&callback_);
  EXPECT_EQ(encoder->Encode(Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(software_->inits, 1);
  EXPECT_EQ(software_->encodes, 1);
  EXPECT_FALSE(encoder->GetEncoderInfo().is_hardware_accelerated);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_OK);
}

TEST_P(VendorCloseStatusTest,
       TypedProcessRetirementFailureDuringOpenCannotStartSoftwareFallback) {
  // A newly blocked instance owns no failed cleanup itself. Its typed Open
  // result must still poison Release so the wrapper cannot bypass the process
  // retirement failure by selecting software.
  retirement_->open_result = Status::kRetirementFailed;
  auto encoder = Wrapper();
  ASSERT_NE(encoder, nullptr);
  EXPECT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(retirement_->close_result, Status::kOk);
  EXPECT_EQ(software_->inits, 0);
  EXPECT_EQ(software_->encodes, 0);
  EXPECT_TRUE(encoder->GetEncoderInfo().is_hardware_accelerated);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  retirement_->open_result = Status::kOk;
  EXPECT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(retirement_->opens, 1);
  EXPECT_EQ(software_->inits, 0);
}

TEST_P(VendorCloseStatusTest,
       TypedProcessRetirementFailureDuringEncodeCannotActivateSoftware) {
  auto encoder = Wrapper();
  ASSERT_NE(encoder, nullptr);
  ASSERT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_OK);
  encoder->RegisterEncodeCompleteCallback(&callback_);
  retirement_->encode_result = Status::kRetirementFailed;
  EXPECT_NE(encoder->Encode(Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(retirement_->close_result, Status::kOk);
  // The wrapper may prepare a software candidate, but must retire it without
  // submitting a frame when the adapter's Release exposes the typed failure.
  EXPECT_EQ(software_->inits, 1);
  EXPECT_EQ(software_->releases, 1);
  EXPECT_EQ(software_->encodes, 0);
  EXPECT_TRUE(encoder->GetEncoderInfo().is_hardware_accelerated);
  EXPECT_EQ(encoder->Encode(Frame(), nullptr), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(Init(*encoder), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(software_->inits, 1);
}

INSTANTIATE_TEST_SUITE_P(CodecRetirement,
                         VendorCloseStatusTest,
                         ::testing::Values(Codec::kH264
#if defined(RTC_ENABLE_H265)
                                           ,
                                           Codec::kH265
#endif
                                           ));

}  // namespace
}  // namespace webrtc
