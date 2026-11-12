/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "common_video/h265/h265_common.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/buffer.h"
#include "test/gtest.h"

namespace webrtc {
namespace {
using external_encoder::Status;

std::vector<uint8_t> HexBytes(std::string_view text) {
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < text.size(); i += 2)
    bytes.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
  return bytes;
}

// Real libx265 Main/tier0/level93, 32x16, 8-bit, one-layer fixture. Independent
// FFmpeg software decoding of key + delta reproduces both input frames exactly.
// These bytes exercise the adapter; they are not hardware evidence.
std::vector<uint8_t> KeyBytes() {
  return HexBytes(
      "0000000140010c01ffff01600000030090000003000003005dba0240"
      "0000000142010101600000030090000003000003005da0421165baabc2e6808000000300"
      "8000000f04"
      "000000014401c073c089"
      "000000012801ac1ae0efdccffa96bfffff6c87e87ebead02ffc0");
}
std::vector<uint8_t> DeltaBytes() {
  return HexBytes("000000010201d0097880d7feddf0");
}

void RemoveNal(std::vector<uint8_t>& bytes, H265::NaluType type) {
  for (const auto& nalu : H265::FindNaluIndices(bytes)) {
    if (H265::ParseNaluType(bytes[nalu.payload_start_offset]) == type) {
      bytes.erase(
          bytes.begin() + nalu.start_offset,
          bytes.begin() + nalu.payload_start_offset + nalu.payload_size);
      return;
    }
  }
}

void MutateSps(std::vector<uint8_t>& bytes,
               const std::function<void(std::vector<uint8_t>&)>& mutate) {
  for (const auto& nalu : H265::FindNaluIndices(bytes)) {
    if (H265::ParseNaluType(bytes[nalu.payload_start_offset]) != H265::kSps)
      continue;
    const size_t begin = nalu.payload_start_offset + 2;
    const size_t end = nalu.payload_start_offset + nalu.payload_size;
    auto rbsp = H265::ParseRbsp(
        ArrayView<const uint8_t>(bytes).subview(begin, end - begin));
    mutate(rbsp);
    Buffer escaped;
    H265::WriteRbsp(rbsp, &escaped);
    bytes.erase(bytes.begin() + begin, bytes.begin() + end);
    bytes.insert(bytes.begin() + begin, escaped.data(),
                 escaped.data() + escaped.size());
    return;
  }
  FAIL() << "Fixture has no SPS";
}

struct H265State {
  external_encoder::Config config;
  external_encoder::Rates rates;
  Status open_status = Status::kOk;
  Status encode_status = Status::kOk;
  Status rates_status = Status::kOk;
  bool forced = false;
  bool wrong_id = false;
  int encodes = 0;
  int closes = 0;
  std::optional<bool> key_override;
  std::function<void(std::vector<uint8_t>&)> mutate;
};

class H265FakeBackend final : public external_encoder::VideoEncoderBackend {
 public:
  explicit H265FakeBackend(std::shared_ptr<H265State> state)
      : state_(std::move(state)) {}
  external_encoder::Info GetInfo() const override {
    return {"Fake HEVC backend", true};
  }
  Status Open(const external_encoder::Config& config) override {
    state_->config = config;
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
    state_->forced = force_key;
    if (state_->encode_status != Status::kOk)
      return state_->encode_status;
    packet->frame_id = frame.id + state_->wrong_id;
    packet->key_frame = state_->key_override.value_or(force_key);
    packet->bytes = force_key ? KeyBytes() : DeltaBytes();
    packet->qp = 24;
    if (state_->mutate)
      state_->mutate(packet->bytes);
    return Status::kOk;
  }
  void Close() override { ++state_->closes; }
  std::string_view LastError() const override {
    return "injected HEVC failure";
  }

 private:
  std::shared_ptr<H265State> state_;
};

class H265Collector final : public EncodedImageCallback {
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

SdpVideoFormat H265Format(std::string level = "93") {
  return SdpVideoFormat("H265", {{"profile-id", "1"},
                                 {"tier-flag", "0"},
                                 {"level-id", std::move(level)},
                                 {"tx-mode", "SRST"}});
}
VideoCodec H265Settings() {
  VideoCodec codec;
  codec.codecType = kVideoCodecH265;
  codec.width = 32;
  codec.height = 16;
  codec.startBitrate = 64;
  codec.maxFramerate = 15;
  codec.active = true;
  return codec;
}
VideoFrame H265Frame(uint32_t timestamp = 9000) {
  auto buffer = I420Buffer::Create(32, 16);
  buffer->InitializeData();
  return VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_rtp_timestamp(timestamp)
      .set_timestamp_us(123000)
      .set_ntp_time_ms(456)
      .set_rotation(kVideoRotation_90)
      .build();
}

class BackendVideoEncoderH265Test : public ::testing::Test {
 protected:
  void SetUp() override { Create(H265Format()); }
  void TearDown() override {
    if (encoder_)
      encoder_->Release();
  }
  void Create(const SdpVideoFormat& format,
              BackendFailurePolicy policy = BackendFailurePolicy::kDefault) {
    encoder_ = CreateBackendVideoEncoder(
        format, std::make_unique<H265FakeBackend>(state_), policy);
    ASSERT_NE(encoder_, nullptr);
    encoder_->RegisterEncodeCompleteCallback(&collector_);
  }
  int Init() {
    return encoder_->InitEncode(
        &codec_,
        VideoEncoder::Settings(VideoEncoder::Capabilities(false), 1, 1200));
  }
  void Rates(uint32_t bps) {
    VideoBitrateAllocation allocation;
    allocation.SetBitrate(0, 0, bps);
    encoder_->SetRates(VideoEncoder::RateControlParameters(allocation, 15));
  }
  void ExpectRejected(std::function<void(std::vector<uint8_t>&)> mutate) {
    state_->mutate = std::move(mutate);
    ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
    EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr),
              WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
    EXPECT_TRUE(collector_.images.empty());
    const int submitted = state_->encodes;
    EXPECT_EQ(encoder_->Encode(H265Frame(12000), nullptr),
              WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
    EXPECT_EQ(state_->encodes, submitted);
  }
  std::shared_ptr<H265State> state_ = std::make_shared<H265State>();
  VideoCodec codec_ = H265Settings();
  H265Collector collector_;
  std::unique_ptr<VideoEncoder> encoder_;
};

TEST_F(BackendVideoEncoderH265Test, PreservesH265MetadataAndOwnsOutput) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state_->config.codec, external_encoder::Codec::kH265);
  EXPECT_EQ(state_->config.h265_level, 93);
  ASSERT_EQ(encoder_->Encode(H265Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(collector_.images.size(), 1u);
  ASSERT_EQ(collector_.infos.size(), 1u);
  EXPECT_EQ(collector_.infos[0].codecType, kVideoCodecH265);
  EXPECT_EQ(collector_.images[0].RtpTimestamp(), 9000u);
  EXPECT_EQ(collector_.images[0].capture_time_ms_, 123);
  EXPECT_EQ(collector_.images[0].NtpTimeMs(), 456);
  EXPECT_EQ(collector_.images[0].rotation(), kVideoRotation_90);
  EXPECT_EQ(collector_.images[0]._encodedWidth, 32u);
  EXPECT_EQ(collector_.images[0]._encodedHeight, 16u);
  EXPECT_EQ(collector_.images[0]._frameType, VideoFrameType::kVideoFrameKey);
  const auto saved = KeyBytes();
  ASSERT_EQ(encoder_->Encode(H265Frame(12000), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_FALSE(state_->forced);
  EXPECT_EQ(collector_.images[1]._frameType, VideoFrameType::kVideoFrameDelta);
  EXPECT_EQ(std::vector<uint8_t>(collector_.images[0].begin(),
                                 collector_.images[0].end()),
            saved);
}

TEST_F(BackendVideoEncoderH265Test, SameCodecFallbackRequiresExplicitOptIn) {
  Create(H265Format(), BackendFailurePolicy::kSameCodecSoftwareFallback);
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->encode_status = Status::kUnavailable;
  EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_TRUE(collector_.images.empty());
  const int submissions = state_->encodes;
  EXPECT_EQ(encoder_->Encode(H265Frame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(state_->encodes, submissions);
}

TEST_F(BackendVideoEncoderH265Test,
       RateFailureRequestsOptedInFallbackOnNextEncode) {
  Create(H265Format(), BackendFailurePolicy::kSameCodecSoftwareFallback);
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(H265Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  state_->rates_status = Status::kError;
  Rates(64000);
  const int submissions = state_->encodes;
  EXPECT_EQ(encoder_->Encode(H265Frame(12000), nullptr),
            WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(state_->encodes, submissions);
}

TEST_F(BackendVideoEncoderH265Test,
       ProcessRetirementFailureIsStickyForOptedInFallback) {
  Create(H265Format(), BackendFailurePolicy::kSameCodecSoftwareFallback);
  state_->open_status = Status::kRetirementFailed;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  state_->open_status = Status::kOk;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_ERROR);
  EXPECT_EQ(state_->encodes, 0);
}

TEST_F(BackendVideoEncoderH265Test, RejectsUnsupportedSdp) {
  EXPECT_TRUE(IsBackendH265FormatSupported(SdpVideoFormat("H265")));
  EXPECT_TRUE(IsNvencH265FormatSupported(SdpVideoFormat("H265")));
  auto mixed = H265Format();
  mixed.parameters["tx-mode"] = "sRsT";
  EXPECT_TRUE(IsBackendH265FormatSupported(mixed));
  for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
           {"profile-id", "2"},
           {"tier-flag", "1"},
           {"level-id", "94"},
           {"tx-mode", "MRST"},
           {"sprop-max-don-diff", "1"}}) {
    auto format = H265Format();
    format.parameters[pair.first] = pair.second;
    EXPECT_FALSE(IsBackendH265FormatSupported(format));
    EXPECT_EQ(CreateBackendVideoEncoder(
                  format, std::make_unique<H265FakeBackend>(state_)),
              nullptr);
  }
}

TEST_F(BackendVideoEncoderH265Test, ForwardsAllNegotiatedLevels) {
  for (int level :
       {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186}) {
    SCOPED_TRACE(level);
    Create(H265Format(std::to_string(level)));
    ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
    EXPECT_EQ(state_->config.h265_level, level);
    encoder_->Release();
  }
}

TEST_F(BackendVideoEncoderH265Test, PausedLevelOneUsesSafeBitrate) {
  Create(H265Format("30"));
  codec_.startBitrate = 0;
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state_->config.h265_level, 30);
  EXPECT_EQ(state_->config.bitrate_bps, 64000u);
  EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state_->encodes, 0);
}

TEST_F(BackendVideoEncoderH265Test, PauseResumeAndNoOutputRetainKeyRequest) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->encode_status = Status::kNoOutput;
  EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(collector_.images.empty());
  state_->encode_status = Status::kOk;
  EXPECT_EQ(encoder_->Encode(H265Frame(12000), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(state_->forced);
  Rates(0);
  const int submitted = state_->encodes;
  EXPECT_EQ(encoder_->Encode(H265Frame(15000), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(state_->encodes, submitted);
  Rates(64000);
  EXPECT_EQ(encoder_->Encode(H265Frame(18000), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(state_->forced);
  EXPECT_EQ(state_->rates.bitrate_bps, 64000u);
}

TEST_F(BackendVideoEncoderH265Test, ExplicitKeyAndReinitializationRequireIdr) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(H265Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  const std::vector<VideoFrameType> key = {VideoFrameType::kVideoFrameKey};
  ASSERT_EQ(encoder_->Encode(H265Frame(12000), &key), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(state_->forced);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(encoder_->Encode(H265Frame(15000), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_TRUE(state_->forced);
}

TEST_F(BackendVideoEncoderH265Test, MissingParameterSetsAreRejected) {
  for (auto type : {H265::kVps, H265::kSps, H265::kPps}) {
    SCOPED_TRACE(type);
    ExpectRejected([type](auto& bytes) { RemoveNal(bytes, type); });
  }
}

TEST_F(BackendVideoEncoderH265Test, RejectsIncorrectSpsContract) {
  for (int kind = 0; kind < 4; ++kind) {
    SCOPED_TRACE(kind);
    ExpectRejected([kind](auto& bytes) {
      MutateSps(bytes, [kind](auto& rbsp) {
        if (kind == 0)
          rbsp[1] = 2;  // Main10.
        if (kind == 1)
          rbsp[1] |= 0x20;  // High tier.
        if (kind == 2)
          rbsp[12] = 120;  // Above negotiated level93.
        if (kind == 3)
          rbsp[12] = 94;  // Undefined level.
      });
    });
  }
}

TEST_F(BackendVideoEncoderH265Test, RejectsNonEightBitChromaWithEightBitLuma) {
  ExpectRejected([](auto& bytes) {
    MutateSps(bytes, [](auto& rbsp) {
      std::vector<bool> bits;
      for (uint8_t byte : rbsp)
        for (int bit = 7; bit >= 0; --bit)
          bits.push_back((byte >> bit) & 1);
      // Fixture bit130 is UE(0) for chroma depth. Replace 1 with UE(1)=010.
      bits.erase(bits.begin() + 130);
      bits.insert(bits.begin() + 130, {false, true, false});
      rbsp.assign((bits.size() + 7) / 8, 0);
      for (size_t bit = 0; bit < bits.size(); ++bit)
        rbsp[bit / 8] |= bits[bit] << (7 - bit % 8);
    });
  });
}

TEST_F(BackendVideoEncoderH265Test, RejectsBadNalHeadersAndReservedPictures) {
  for (int kind = 0; kind < 7; ++kind) {
    SCOPED_TRACE(kind);
    ExpectRejected([kind](auto& bytes) {
      const auto nalu = H265::FindNaluIndices(bytes).back();
      const size_t offset = nalu.payload_start_offset;
      if (kind == 0)
        bytes[offset] |= 0x80;  // forbidden_zero_bit.
      if (kind == 1)
        bytes[offset + 1] |= 8;  // Nonzero layer.
      if (kind == 2)
        bytes[offset + 1] = 0;  // temporal_id_plus1=0.
      if (kind == 3)
        bytes[offset + 1] = 2;  // Additional temporal layer.
      if (kind == 4)
        bytes[offset] = 10 << 1;  // Reserved VCL.
      if (kind == 5)
        bytes[offset] = 21 << 1;  // CRA is not an IDR.
      if (kind == 6)
        bytes.resize(offset + 2);  // Header-only slice.
    });
  }
}

TEST_F(BackendVideoEncoderH265Test, RejectsRtpWrappersInsideAnnexB) {
  for (int type : {48, 49, 50}) {
    SCOPED_TRACE(type);
    ExpectRejected([type](auto& bytes) {
      bytes.insert(bytes.end(),
                   {0, 0, 0, 1, static_cast<uint8_t>(type << 1), 1, 0x80});
    });
  }
}

TEST_F(BackendVideoEncoderH265Test, RejectsIdrFlagMismatchAndWrongFrameId) {
  state_->key_override = false;
  ExpectRejected([](auto&) {});
  state_->key_override.reset();
  state_->wrong_id = true;
  ExpectRejected([](auto&) {});
}

TEST_F(BackendVideoEncoderH265Test, RequiresVclAndRequestedKeyPicture) {
  ExpectRejected([](auto& bytes) { RemoveNal(bytes, H265::kIdrNLp); });
  state_->key_override = false;
  ExpectRejected([](auto& bytes) { bytes = DeltaBytes(); });
}

TEST_F(BackendVideoEncoderH265Test, ReportsBackendFailureWithoutH264Fallback) {
  state_->open_status = Status::kUnavailable;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
  state_->open_status = Status::kOk;
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->encode_status = Status::kError;
  EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr),
            WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
  state_->encode_status = Status::kOk;
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  state_->rates_status = Status::kError;
  Rates(128000);
  EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr),
            WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
}

TEST_F(BackendVideoEncoderH265Test, RejectsLayeredConfiguration) {
  codec_.numberOfSimulcastStreams = 2;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
  codec_.numberOfSimulcastStreams = 0;
  codec_.simulcastStream[0].numberOfTemporalLayers = 2;
  EXPECT_EQ(Init(), WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
}

TEST_F(BackendVideoEncoderH265Test,
       AllowsReleaseInCallbackAndRetainsOwnedBytes) {
  ASSERT_EQ(Init(), WEBRTC_VIDEO_CODEC_OK);
  collector_.on_encoded = [this] {
    EXPECT_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  };
  ASSERT_EQ(encoder_->Encode(H265Frame(), nullptr), WEBRTC_VIDEO_CODEC_OK);
  ASSERT_EQ(collector_.images.size(), 1u);
  EXPECT_EQ(std::vector<uint8_t>(collector_.images[0].begin(),
                                 collector_.images[0].end()),
            KeyBytes());
  EXPECT_EQ(encoder_->Encode(H265Frame(), nullptr),
            WEBRTC_VIDEO_CODEC_UNINITIALIZED);
}
}  // namespace
}  // namespace webrtc
