/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

// Test-only fault injection around the real AMD backend. A successful test
// proves same-codec software fallback and subsequent real hardware recovery,
// not physical device removal, driver reset or device-lost recovery.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "common_video/h264/h264_common.h"
#if defined(RTC_ENABLE_H265)
#include "common_video/h265/h265_common.h"
#include "modules/video_coding/codecs/external/ffmpeg_h265_test_decoder.h"
#include "modules/video_coding/codecs/external/x265_video_encoder.h"
#endif
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {
using external_encoder::Status;
constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kHardwareFrames = 4;
constexpr int kSoftwareFrames = 8;
constexpr int kRecoveryFrames = 5;
constexpr double kMinimumPsnr = 28.0;

std::string Json(std::string_view input) {
  std::string escaped = "\"";
  for (const unsigned char value : input) {
    switch (value) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (value < 0x20) {
          constexpr char digits[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped += digits[value >> 4];
          escaped += digits[value & 15];
        } else {
          escaped += static_cast<char>(value);
        }
    }
  }
  return escaped + "\"";
}

enum class Fault { kNone, kEncode, kSetRates, kOpenUnavailable };
const char* FaultName(Fault fault) {
  switch (fault) {
    case Fault::kNone:
      return "none";
    case Fault::kEncode:
      return "Encode";
    case Fault::kSetRates:
      return "SetRates";
    case Fault::kOpenUnavailable:
      return "OpenUnavailable";
  }
  return "invalid";
}

struct FaultState {
  Fault armed = Fault::kNone;
  int injections = 0;
  int successful_opens = 0;
  int successful_hardware_encodes = 0;
  int hardware_encode_calls = 0;
  int close_calls = 0;
  bool active = false;
  std::string injected_reason;
};

class FaultInjectingBackend final
    : public external_encoder::VideoEncoderBackend {
 public:
  FaultInjectingBackend(
      std::unique_ptr<external_encoder::VideoEncoderBackend> backend,
      std::shared_ptr<FaultState> state)
      : backend_(std::move(backend)), state_(std::move(state)) {}
  external_encoder::Info GetInfo() const override {
    return backend_->GetInfo();
  }
  Status Open(const external_encoder::Config& config) override {
    if (Inject(Fault::kOpenUnavailable)) {
      backend_->Close();
      state_->active = false;
      return Status::kUnavailable;
    }
    error_.clear();
    const auto result = backend_->Open(config);
    state_->active = result == Status::kOk;
    state_->successful_opens += state_->active;
    return result;
  }
  Status SetRates(const external_encoder::Rates& rates) override {
    if (Inject(Fault::kSetRates))
      return Status::kError;
    error_.clear();
    return backend_->SetRates(rates);
  }
  Status Encode(const external_encoder::Frame& frame,
                bool key,
                external_encoder::Packet* packet) override {
    if (Inject(Fault::kEncode))
      return Status::kError;
    error_.clear();
    ++state_->hardware_encode_calls;
    const auto result = backend_->Encode(frame, key, packet);
    state_->successful_hardware_encodes += result == Status::kOk;
    return result;
  }
  void Close() override {
    backend_->Close();
    ++state_->close_calls;
    state_->active = false;
  }
  Status LastCloseStatus() const override {
    return backend_->LastCloseStatus();
  }
  std::string_view LastError() const override {
    return error_.empty() ? backend_->LastError() : std::string_view(error_);
  }

 private:
  bool Inject(Fault operation) {
    if (state_->armed != operation)
      return false;
    state_->armed = Fault::kNone;
    ++state_->injections;
    error_ = std::string("TEST_INJECTED_") + FaultName(operation);
    state_->injected_reason = error_;
    return true;
  }
  std::unique_ptr<external_encoder::VideoEncoderBackend> backend_;
  const std::shared_ptr<FaultState> state_;
  std::string error_;
};

struct SoftwareState {
  int successful_inits = 0;
  int encodes = 0;
  int rates_calls = 0;
  uint32_t last_bitrate_bps = 0;
  double last_fps = 0;
};

// Observe forwarding of the wrapper's cached rates without replacing the real
// OpenH264/x265 software encoder or its outputs.
class ObservedSoftwareEncoder final : public VideoEncoder {
 public:
  ObservedSoftwareEncoder(std::unique_ptr<VideoEncoder> encoder,
                          std::shared_ptr<SoftwareState> state)
      : encoder_(std::move(encoder)), state_(std::move(state)) {}
  int InitEncode(const VideoCodec* codec, const Settings& settings) override {
    const int result = encoder_->InitEncode(codec, settings);
    state_->successful_inits += result == WEBRTC_VIDEO_CODEC_OK;
    return result;
  }
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override {
    return encoder_->RegisterEncodeCompleteCallback(callback);
  }
  int32_t Release() override { return encoder_->Release(); }
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* types) override {
    ++state_->encodes;
    return encoder_->Encode(frame, types);
  }
  void SetRates(const RateControlParameters& rates) override {
    ++state_->rates_calls;
    state_->last_bitrate_bps = rates.bitrate.get_sum_bps();
    state_->last_fps = rates.framerate_fps;
    encoder_->SetRates(rates);
  }
  EncoderInfo GetEncoderInfo() const override {
    return encoder_->GetEncoderInfo();
  }

 private:
  std::unique_ptr<VideoEncoder> encoder_;
  const std::shared_ptr<SoftwareState> state_;
};

struct Encoded {
  EncodedImage image;
  std::optional<CodecSpecificInfo> info;
};
class EncodedCollector final : public EncodedImageCallback {
 public:
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++callbacks;
    if (!accepting_ || owner_ != std::this_thread::get_id()) {
      ++unexpected;
      return Result(Result::ERROR_SEND_FAILED);
    }
    values_.push_back({image, info ? std::make_optional(*info) : std::nullopt});
    return Result(Result::OK, image.RtpTimestamp());
  }
  void OnDroppedFrame(DropReason) override { ++dropped; }
  void Begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    accepting_ = true;
  }
  std::vector<Encoded> End() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    return std::move(values_);
  }
  std::atomic<int> callbacks{0}, unexpected{0}, dropped{0};

 private:
  const std::thread::id owner_ = std::this_thread::get_id();
  std::mutex mutex_;
  bool accepting_ = false;
  std::vector<Encoded> values_;
};

class DecodedCollector final : public DecodedImageCallback {
 public:
  int32_t Decoded(VideoFrame& frame) override {
    frames.push_back(frame);
    return 0;
  }
  void Decoded(VideoFrame& frame,
               std::optional<int32_t>,
               std::optional<uint8_t>) override {
    frames.push_back(frame);
  }
  std::vector<VideoFrame> frames;
};

class ReferenceDecoder {
 public:
  ReferenceDecoder(VideoCodecType codec, std::string directory)
      : codec_(codec), directory_(std::move(directory)) {}
  ~ReferenceDecoder() {
    if (!Release())
      std::cerr << "FAIL reference decoder destructor Release\n";
  }
  bool Reset() {
    if (!Release())
      return false;
    if (codec_ == kVideoCodecH264) {
      decoder_ = H264Decoder::Create();
    } else {
#if defined(RTC_ENABLE_H265)
      decoder_ = std::make_unique<FfmpegH265TestDecoder>(directory_);
#endif
    }
    if (!decoder_)
      return false;
    VideoDecoder::Settings settings;
    settings.set_codec_type(codec_);
    settings.set_max_render_resolution({kWidth, kHeight});
    settings.set_number_of_cores(1);
    const bool configured = decoder_->Configure(settings) &&
                            decoder_->RegisterDecodeCompleteCallback(
                                &collector_) == WEBRTC_VIDEO_CODEC_OK &&
                            !decoder_->GetDecoderInfo().is_hardware_accelerated;
    const auto identity = decoder_->GetDecoderInfo().implementation_name;
    return configured &&
           (codec_ == kVideoCodecH264
                ? identity == "FFmpeg"
                : identity.find("FFmpeg HEVC test software") == 0);
  }
  bool Decode(const EncodedImage& image,
              const VideoFrame& reference,
              double* psnr) {
    collector_.frames.clear();
    if (!decoder_ || decoder_->Decode(image, -1) != WEBRTC_VIDEO_CODEC_OK ||
        collector_.frames.size() != 1)
      return false;
    const auto& decoded = collector_.frames.front();
    if (decoded.width() != reference.width() ||
        decoded.height() != reference.height() ||
        decoded.rtp_timestamp() != reference.rtp_timestamp())
      return false;
    *psnr = I420PSNR(&reference, &decoded);
    return std::isfinite(*psnr) && *psnr >= kMinimumPsnr;
  }
  bool Release() {
    const bool released =
        !decoder_ || decoder_->Release() == WEBRTC_VIDEO_CODEC_OK;
    decoder_.reset();
    collector_.frames.clear();
    return released;
  }
  std::string Identity() const {
    return decoder_ ? decoder_->GetDecoderInfo().implementation_name : "";
  }

 private:
  const VideoCodecType codec_;
  const std::string directory_;
  std::unique_ptr<VideoDecoder> decoder_;
  DecodedCollector collector_;
};

VideoFrame MakeFrame(int index) {
  auto buffer = I420Buffer::Create(kWidth, kHeight);
  const int square_x = (index * 13) % (kWidth - 80);
  const int square_y = (index * 7) % (kHeight - 80);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const bool square = x >= square_x && x < square_x + 80 && y >= square_y &&
                          y < square_y + 80;
      buffer->MutableDataY()[y * buffer->StrideY() + x] =
          square ? 220 : 32 + ((x / 8 + y / 8) % 128);
    }
  }
  for (int y = 0; y < kHeight / 2; ++y) {
    for (int x = 0; x < kWidth / 2; ++x) {
      buffer->MutableDataU()[y * buffer->StrideU() + x] = 72 + (x / 8) % 32;
      buffer->MutableDataV()[y * buffer->StrideV() + x] = 160 + (y / 8) % 32;
    }
  }
  const uint32_t timestamp = 9000 + static_cast<uint32_t>(index) * 3000;
  return VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_rtp_timestamp(timestamp)
      .set_timestamp_us(static_cast<int64_t>(timestamp) * 1000000 / 90000)
      .build();
}

VideoCodec Settings(VideoCodecType type) {
  VideoCodec codec;
  codec.codecType = type;
  codec.width = kWidth;
  codec.height = kHeight;
  codec.startBitrate = 2000;
  codec.minBitrate = 30;
  codec.maxBitrate = 4000;
  codec.maxFramerate = 30;
  codec.active = true;
  codec.numberOfSimulcastStreams = 0;
  codec.SetFrameDropEnabled(false);
  if (type == kVideoCodecH264) {
    *codec.H264() = VideoEncoder::GetDefaultH264Settings();
    codec.H264()->numberOfTemporalLayers = 1;
    codec.H264()->keyFrameInterval = 300;
  }
  return codec;
}

bool CheckPacket(const Encoded& encoded,
                 VideoCodecType codec,
                 const VideoFrame& reference,
                 bool require_key) {
  const auto& image = encoded.image;
  if (!encoded.info || encoded.info->codecType != codec || !image.size() ||
      image.RtpTimestamp() != reference.rtp_timestamp() ||
      image._encodedWidth != kWidth || image._encodedHeight != kHeight)
    return false;
  bool idr = false, sps = false, pps = false, vps = codec == kVideoCodecH264;
  const ArrayView<const uint8_t> bytes(image.data(), image.size());
  if (codec == kVideoCodecH264) {
    for (const auto& nalu : H264::FindNaluIndices(bytes)) {
      if (!nalu.payload_size)
        return false;
      const auto type = H264::ParseNaluType(bytes[nalu.payload_start_offset]);
      idr |= type == H264::kIdr;
      sps |= type == H264::kSps;
      pps |= type == H264::kPps;
    }
    if (encoded.info->codecSpecific.H264.packetization_mode !=
        H264PacketizationMode::NonInterleaved)
      return false;
  } else {
#if defined(RTC_ENABLE_H265)
    for (const auto& nalu : H265::FindNaluIndices(bytes)) {
      if (nalu.payload_size < 2)
        return false;
      const auto type = H265::ParseNaluType(bytes[nalu.payload_start_offset]);
      idr |= type == H265::kIdrNLp || type == H265::kIdrWRadl;
      sps |= type == H265::kSps;
      pps |= type == H265::kPps;
      vps |= type == H265::kVps;
    }
#endif
  }
  return (image._frameType == VideoFrameType::kVideoFrameKey) == idr &&
         (!require_key || (idr && sps && pps && vps));
}

struct CaseResult {
  std::string name;
  bool passed = false;
  std::string failure;
  std::string hardware;
  std::string software;
  std::string reason;
  std::string decoder;
  int initial_hardware_frames = 0;
  int software_frames = 0;
  int recovered_hardware_frames = 0;
  int injections = 0;
  int software_inits = 0;
  int software_rates_calls = 0;
  int closes = 0;
  int unexpected_callbacks = 0;
  int drops = 0;
  double minimum_psnr = 100.0;
};

CaseResult RunCase(const Environment& env,
                   VideoCodecType codec,
                   Fault fault,
                   const std::string& ffmpeg_directory,
                   const std::filesystem::path& output_directory) {
  CaseResult result;
  result.name = FaultName(fault);
  auto state = std::make_shared<FaultState>();
  auto software_state = std::make_shared<SoftwareState>();
  EncodedCollector collector;
  ReferenceDecoder decoder(codec, ffmpeg_directory);
  const auto format = codec == kVideoCodecH264
                          ? CreateH264Format(H264Profile::kProfileBaseline,
                                             H264Level::kLevel3_1, "1")
                          : SdpVideoFormat("H265", {{"profile-id", "1"},
                                                    {"tier-flag", "0"},
                                                    {"level-id", "93"},
                                                    {"tx-mode", "SRST"}});
  std::unique_ptr<external_encoder::VideoEncoderBackend> real_backend;
  std::unique_ptr<VideoEncoder> real_software;
  if (codec == kVideoCodecH264) {
    real_backend = external_encoder::CreateAmfH264Backend();
    real_software = CreateH264Encoder(env, H264EncoderSettings::Parse(format));
  } else {
#if defined(RTC_ENABLE_H265)
    real_backend = external_encoder::CreateAmfH265Backend();
    real_software = CreateX265VideoEncoder(env, format);
#endif
  }
  if (!real_backend || !real_software) {
    result.failure = "real AMF or same-codec software construction failed";
    return result;
  }
  auto hardware = CreateBackendVideoEncoder(
      format,
      std::make_unique<FaultInjectingBackend>(std::move(real_backend), state),
      BackendFailurePolicy::kSameCodecSoftwareFallback);
  if (!hardware) {
    result.failure = "hardware adapter construction failed";
    return result;
  }
  result.hardware = hardware->GetEncoderInfo().implementation_name;
  result.software = real_software->GetEncoderInfo().implementation_name;
  auto encoder = CreateVideoEncoderSoftwareFallbackWrapper(
      env,
      std::make_unique<ObservedSoftwareEncoder>(std::move(real_software),
                                                software_state),
      std::move(hardware), false);
  const auto codec_settings = Settings(codec);
  const VideoEncoder::Settings encoder_settings(
      VideoEncoder::Capabilities(false), 4, 1200);
  std::ofstream frames(output_directory / (result.name + "-frames.jsonl"));
  std::ofstream access_units(
      output_directory /
          (result.name + (codec == kVideoCodecH264 ? ".h264" : ".h265")),
      std::ios::binary);
  auto require = [&](bool condition, std::string failure) {
    if (!condition && result.failure.empty())
      result.failure = std::move(failure);
    return condition;
  };
  auto init = [&] {
    return require(encoder->InitEncode(&codec_settings, encoder_settings) ==
                       WEBRTC_VIDEO_CODEC_OK,
                   "wrapper InitEncode failed") &&
           require(decoder.Reset(), "reference decoder configuration failed");
  };
  auto send = [&](int index, bool expect_hardware, bool key,
                  const char* phase) {
    const VideoFrame reference = MakeFrame(index);
    const std::vector<VideoFrameType> types{
        key ? VideoFrameType::kVideoFrameKey
            : VideoFrameType::kVideoFrameDelta};
    collector.Begin();
    const int status = encoder->Encode(reference, &types);
    auto values = collector.End();
    const auto info = encoder->GetEncoderInfo();
    const std::string expected_identity =
        expect_hardware ? result.hardware : result.software;
    double psnr = 0;
    if (!require(status == WEBRTC_VIDEO_CODEC_OK, "Encode failed") ||
        !require(values.size() == 1,
                 "expected exactly one synchronous callback") ||
        !require(info.is_hardware_accelerated == expect_hardware &&
                     info.implementation_name == expected_identity,
                 "requested/effective encoder identity mismatch") ||
        !require(CheckPacket(values[0], codec, reference, key),
                 "same-codec packet, keyframe or metadata contract failed") ||
        !require(decoder.Decode(values[0].image, reference, &psnr),
                 "actual software reference decode/PSNR failed"))
      return false;
    result.minimum_psnr = std::min(result.minimum_psnr, psnr);
    result.decoder = decoder.Identity();
    const auto& image = values[0].image;
    access_units.write(reinterpret_cast<const char*>(image.data()),
                       image.size());
    frames << "{\"phase\":" << Json(phase)
           << ",\"requested\":\"AMF\",\"effective\":"
           << Json(info.implementation_name) << ",\"hardware\":"
           << (info.is_hardware_accelerated ? "true" : "false")
           << ",\"rtp_timestamp\":" << image.RtpTimestamp()
           << ",\"encoded_bytes\":" << image.size() << ",\"keyframe\":"
           << (image._frameType == VideoFrameType::kVideoFrameKey ? "true"
                                                                  : "false")
           << ",\"decode_psnr_db\":" << std::setprecision(10) << psnr
           << ",\"fallback_reason\":"
           << Json(expect_hardware ? "" : state->injected_reason) << "}\n";
    if (std::string_view(phase) == "initial_hardware")
      ++result.initial_hardware_frames;
    else if (std::string_view(phase) == "software_fallback")
      ++result.software_frames;
    else
      ++result.recovered_hardware_frames;
    return require(frames.good() && access_units.good(),
                   "evidence output failed");
  };
  auto release = [&] {
    const int before = collector.callbacks;
    const auto frame = MakeFrame(100);
    const bool ok =
        require(encoder->Release() == WEBRTC_VIDEO_CODEC_OK &&
                    encoder->Release() == WEBRTC_VIDEO_CODEC_OK,
                "idempotent wrapper Release failed") &&
        require(!state->active, "hardware session active after Release") &&
        require(encoder->Encode(frame, nullptr) != WEBRTC_VIDEO_CODEC_OK,
                "Encode succeeded after Release");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return ok &&
           require(collector.callbacks == before && collector.unexpected == 0,
                   "callback delivered after Release or on wrong sequence");
  };

  bool ok =
      require(frames.good() && access_units.good(),
              "evidence files unavailable") &&
      require(result.hardware.find("AMF ") == 0 &&
                  result.software.find(codec == kVideoCodecH264 ? "OpenH264"
                                                                : "x265") == 0,
              "unexpected real hardware/software identity") &&
      require(encoder->RegisterEncodeCompleteCallback(&collector) ==
                  WEBRTC_VIDEO_CODEC_OK,
              "callback registration failed") &&
      init();
  for (int i = 0; ok && i < kHardwareFrames; ++i)
    ok = send(i, true, i == 0, "initial_hardware");
  if (ok) {
    state->armed = fault;
    if (fault == Fault::kOpenUnavailable) {
      // Keep the exact same wrapper and backend objects across Release/Init.
      ok = release() && init();
    } else if (fault == Fault::kSetRates) {
      VideoBitrateAllocation bitrate;
      ok = require(bitrate.SetBitrate(0, 0, 1200000), "rate allocation failed");
      if (ok)
        encoder->SetRates(VideoEncoder::RateControlParameters(bitrate, 15.0));
    }
  }
  for (int i = 0; ok && i < kSoftwareFrames; ++i)
    ok = send(kHardwareFrames + i, false, i == 0, "software_fallback");
  if (ok) {
    result.reason = state->injected_reason;
    ok = require(state->injections == 1 &&
                     result.reason ==
                         std::string("TEST_INJECTED_") + FaultName(fault),
                 "exactly one documented fault injection required") &&
         require(state->successful_hardware_encodes == kHardwareFrames &&
                     state->hardware_encode_calls == kHardwareFrames,
                 "hardware submitted frames after fault") &&
         require(
             software_state->successful_inits == 1 &&
                 software_state->encodes == kSoftwareFrames,
             "real software fallback was not invoked exactly once per frame") &&
         require(!state->active, "wrapper did not release failed AMF backend");
    if (fault == Fault::kSetRates)
      ok =
          ok &&
          require(software_state->rates_calls >= 1 &&
                      software_state->last_bitrate_bps == 1200000 &&
                      software_state->last_fps == 15,
                  "cached SetRates were not replayed to real software encoder");
  }
  if (ok) {
    std::cout << "FALLBACK requested=AMF effective=" << result.software
              << " reason=" << result.reason << " fault_model=TEST_INJECTED\n";
    ok = release() && init();
  }
  for (int i = 0; ok && i < kRecoveryFrames; ++i)
    ok = send(kHardwareFrames + kSoftwareFrames + i, true, i == 0,
              "recovered_hardware");
  if (ok)
    ok =
        require(state->successful_hardware_encodes ==
                        kHardwareFrames + kRecoveryFrames &&
                    software_state->encodes == kSoftwareFrames &&
                    state->injections == 1,
                "Release/Init did not recover the original real AMF backend") &&
        release();
  // Also release on failure; keep this teardown separate from the PASS gates.
  const bool encoder_released =
      require(encoder->Release() == WEBRTC_VIDEO_CODEC_OK,
              "final wrapper Release failed");
  const bool decoder_released =
      require(decoder.Release(), "final reference decoder Release failed");
  ok = ok && encoder_released && decoder_released;
  result.injections = state->injections;
  result.software_inits = software_state->successful_inits;
  result.software_rates_calls = software_state->rates_calls;
  result.closes = state->close_calls;
  result.unexpected_callbacks = collector.unexpected;
  result.drops = collector.dropped;
  result.passed = ok && result.failure.empty() &&
                  result.unexpected_callbacks == 0 && result.drops == 0 &&
                  result.initial_hardware_frames == kHardwareFrames &&
                  result.software_frames == kSoftwareFrames &&
                  result.recovered_hardware_frames == kRecoveryFrames;
  if (!result.passed && result.failure.empty())
    result.failure =
        "frame counts, drop count or callback count did not satisfy gate";
  std::cout << (result.passed ? "PASS " : "FAIL ") << "injected_" << result.name
            << " actual_hw=" << result.initial_hardware_frames
            << " actual_sw=" << result.software_frames
            << " recovered_hw=" << result.recovered_hardware_frames
            << " minimum_psnr=" << result.minimum_psnr
            << " failure=" << result.failure << '\n';
  return result;
}

int Run(int argc, char** argv) {
  VideoCodecType codec = kVideoCodecH264;
  std::string codec_name = "h264", ffmpeg_directory;
  std::filesystem::path output_directory;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (i + 1 >= argc) {
      std::cerr << "missing value for " << argument << '\n';
      return 2;
    }
    const std::string value = argv[++i];
    if (argument == "--codec" && (value == "h264" || value == "h265")) {
      codec_name = value;
      codec = value == "h264" ? kVideoCodecH264 : kVideoCodecH265;
    } else if (argument == "--output-dir") {
      output_directory = std::filesystem::u8path(value);
    } else if (argument == "--ffmpeg-dll-dir") {
      ffmpeg_directory = value;
    } else {
      std::cerr << "invalid argument " << argument << " " << value << '\n';
      return 2;
    }
  }
  if (output_directory.empty()) {
    std::cerr
        << "usage: amf_fallback_smoke --codec h264|h265 --output-dir PATH "
           "[--ffmpeg-dll-dir ABSOLUTE_OBS_DLL_DIRECTORY]\n";
    return 2;
  }
#if !defined(RTC_ENABLE_H265)
  if (codec == kVideoCodecH265) {
    std::cerr << "H265 is not compiled; NOT_SUPPORTED\n";
    return 3;
  }
#endif
  if (codec == kVideoCodecH265 && ffmpeg_directory.empty()) {
    std::cerr << "H265 requires --ffmpeg-dll-dir for actual reference decode\n";
    return 2;
  }
  std::error_code error;
  std::filesystem::create_directories(output_directory, error);
  if (error) {
    std::cerr << "cannot create evidence directory: " << error.message()
              << '\n';
    return 2;
  }
  // Never overwrite an earlier acceptance report.
  if (std::filesystem::exists(output_directory / "fallback-result.json")) {
    std::cerr << "output directory already contains fallback-result.json\n";
    return 2;
  }
  for (const auto fault :
       {Fault::kEncode, Fault::kSetRates, Fault::kOpenUnavailable}) {
    const std::string name = FaultName(fault);
    if (std::filesystem::exists(output_directory / (name + "-frames.jsonl")) ||
        std::filesystem::exists(output_directory / (name + ".h264")) ||
        std::filesystem::exists(output_directory / (name + ".h265"))) {
      std::cerr << "output directory contains a previous case artifact\n";
      return 2;
    }
  }
  const auto env = CreateEnvironment();
  std::vector<CaseResult> results;
  for (const auto fault :
       {Fault::kEncode, Fault::kSetRates, Fault::kOpenUnavailable})
    results.push_back(
        RunCase(env, codec, fault, ffmpeg_directory, output_directory));
  const bool passed =
      std::all_of(results.begin(), results.end(),
                  [](const auto& result) { return result.passed; });
  std::ofstream report(output_directory / "fallback-result.json");
  report << "{\"schema\":1,\"codec\":" << Json(codec_name)
         << ",\"status\":" << Json(passed ? "PASS" : "FAIL")
         << ",\"validation_scope\":\"real-AMF plus injected-fault same-codec "
            "fallback and recovery\","
            "\"physical_device_lost\":\"NOT_RUN\",\"network_rtp\":\"NOT_RUN\","
            "\"factory_creation_unavailable\":\"NOT_RUN\","
            "\"cases\":[";
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    if (i)
      report << ',';
    report << "{\"name\":" << Json(result.name)
           << ",\"status\":" << Json(result.passed ? "PASS" : "FAIL")
           << ",\"failure\":" << Json(result.failure)
           << ",\"requested\":\"AMF\",\"initial_effective\":"
           << Json(result.hardware)
           << ",\"fallback_effective\":" << Json(result.software)
           << ",\"fallback_reason\":" << Json(result.reason)
           << ",\"decoder\":" << Json(result.decoder)
           << ",\"initial_hardware_frames\":" << result.initial_hardware_frames
           << ",\"software_frames\":" << result.software_frames
           << ",\"recovered_hardware_frames\":"
           << result.recovered_hardware_frames
           << ",\"injections\":" << result.injections
           << ",\"software_inits\":" << result.software_inits
           << ",\"software_rates_calls\":" << result.software_rates_calls
           << ",\"backend_close_calls\":" << result.closes
           << ",\"unexpected_callbacks\":" << result.unexpected_callbacks
           << ",\"dropped_callbacks\":" << result.drops
           << ",\"minimum_psnr_db\":" << std::setprecision(10)
           << result.minimum_psnr << '}';
  }
  report << "]}\n";
  report.close();
  if (!report.good()) {
    std::cerr << "could not save fallback-result.json\n";
    return 2;
  }
  std::cout << "Validation: " << (passed ? "PASS" : "FAIL") << "\nEvidence: "
            << (output_directory / "fallback-result.json").string() << '\n';
  return passed ? 0 : 1;
}
}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
  return webrtc::Run(argc, argv);
}
