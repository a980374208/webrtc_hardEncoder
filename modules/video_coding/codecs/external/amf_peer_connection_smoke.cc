/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
// Test-only, two independent processes with real PeerConnection media stacks.
// Files exchange fully gathered SDP only; media uses real UDP/ICE/DTLS/SRTP.
// Input is synthetic or the actual primary GDI desktop. A local-chain PASS is
// not cross-host or release coverage.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/transport/bitrate_settings.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/h265_profile_tier_level.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "media/base/video_broadcaster.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/ffmpeg_h265_test_decoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/win32_socket_init.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/scale.h"

namespace webrtc {
namespace {
using SteadyClock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

struct Options {
  std::string role, output_directory, codec = "H264", ffmpeg;
  std::string backend = "AMF";
  std::string ffmpeg_dll_directory, source = "synthetic";
  bool drop_diagnostics = false;
  int seconds = 30, width = 1280, height = 720, fps = 30;
  int bitrate() const { return width == 1920 ? 4000000 : 2000000; }
};

std::string Quote(const std::string& input) {
  std::string output = "\"";
  for (unsigned char c : input) {
    if (c == '\\' || c == '"') {
      output += '\\';
      output += static_cast<char>(c);
    } else if (c < 32) {
      const char hex[] = "0123456789abcdef";
      output += "\\u00";
      output += hex[c >> 4];
      output += hex[c & 15];
    } else {
      output += static_cast<char>(c);
    }
  }
  return output + "\"";
}

bool Parse(int argc, char** argv, Options* options) {
  if (argc % 2 != 1)
    return false;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--role")
      options->role = value;
    else if (key == "--outputdir")
      options->output_directory = value;
    else if (key == "--codec")
      options->codec = value;
    else if (key == "--backend")
      options->backend = value;
    else if (key == "--ffmpeg")
      options->ffmpeg = value;
    else if (key == "--ffmpeg-dll-dir")
      options->ffmpeg_dll_directory = value;
    else if (key == "--source")
      options->source = value;
    else if (key == "--drop-diagnostics") {
      if (value != "0" && value != "1")
        return false;
      options->drop_diagnostics = value == "1";
    } else {
      char* end = nullptr;
      const long number = std::strtol(value.c_str(), &end, 10);
      if (value.empty() || !end || *end || number < 1 || number > 3600)
        return false;
      if (key == "--seconds")
        options->seconds = static_cast<int>(number);
      else if (key == "--width")
        options->width = static_cast<int>(number);
      else if (key == "--height")
        options->height = static_cast<int>(number);
      else if (key == "--fps")
        options->fps = static_cast<int>(number);
      else
        return false;
    }
  }
  return (options->role == "sender" || options->role == "receiver") &&
         (options->codec == "H264" || options->codec == "H265") &&
         (options->backend == "AMF" || options->backend == "NVENC" ||
          options->backend == "QSV") &&
         (options->source == "synthetic" || options->source == "desktop") &&
         (options->codec != "H265" ||
          (!options->ffmpeg_dll_directory.empty() &&
           std::filesystem::path(options->ffmpeg_dll_directory)
               .is_absolute())) &&
         !options->output_directory.empty() &&
         std::filesystem::path(options->output_directory).is_absolute() &&
         ((options->width == 1280 && options->height == 720) ||
          (options->width == 1920 && options->height == 1080)) &&
         options->fps == 30;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.close();
  if (!file)
    return false;
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return input.bad() ? std::string() : output.str();
}

// Constant memory for arbitrarily long runs. Percentiles are bucket upper
// bounds (0.1 ms precision), not interval means from RTCStats. Overflow
// percentiles are reported as null, with the overflow count and exact max.
class LatencyHistogram {
 public:
  void Add(double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    sum_ms_ += ms;
    max_ms_ = std::max(max_ms_, ms);
    if (ms > 2000.0)
      ++overflow_;
    else
      ++buckets_[std::min<size_t>(20000,
                                  static_cast<size_t>(std::max(0.0, ms) * 10))];
  }
  std::string Json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << std::setprecision(10) << "{\"count\":" << count_
           << ",\"bucket_ms\":0.1,\"ceiling_ms\":2000,\"overflow\":"
           << overflow_ << ",\"mean_ms\":";
    if (count_)
      output << sum_ms_ / count_;
    else
      output << "null";
    output << ",\"p50_ms\":" << Percentile(0.50)
           << ",\"p95_ms\":" << Percentile(0.95)
           << ",\"p99_ms\":" << Percentile(0.99) << ",\"max_ms\":";
    if (count_)
      output << max_ms_;
    else
      output << "null";
    return output.str() + "}";
  }

 private:
  std::string Percentile(double fraction) const {
    if (!count_)
      return "null";
    const uint64_t rank = static_cast<uint64_t>(std::ceil(count_ * fraction));
    uint64_t cumulative = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      cumulative += buckets_[i];
      if (cumulative >= rank)
        return std::to_string(std::min(2000.0, (i + 1) / 10.0));
    }
    return "null";
  }
  mutable std::mutex mutex_;
  std::array<uint64_t, 20001> buckets_{};
  uint64_t count_ = 0, overflow_ = 0;
  double sum_ms_ = 0, max_ms_ = 0;
};

void UpdatePeak(std::atomic<uint64_t>& peak, uint64_t value) {
  uint64_t previous = peak.load();
  while (previous < value && !peak.compare_exchange_weak(previous, value)) {
  }
}

struct Shared {
  explicit Shared(const Options& value) : options(value) {}
  void Fail(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex);
    if (failure.empty())
      failure = reason;
    failed.store(true);
  }
  void SetEncoderName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex);
    encoder_name = std::move(name);
  }
  void SetDecoderName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex);
    decoder_name = std::move(name);
  }
  const Options options;
  std::mutex mutex;
  std::string failure, encoder_name, decoder_name;
  std::vector<std::string> builtin_encoder_formats;
  std::vector<uint8_t> first_key, last_key;
  std::atomic<bool> failed{false}, closing{false}, local_set{false};
  std::atomic<bool> remote_set{false}, gathered{false}, connected_once{false};
  std::atomic<int> ice_state{0}, connection_state{0}, candidates{0};
  std::atomic<uint64_t> source_frames{0}, skipped_capture_slots{0};
  std::atomic<uint64_t> encode_calls{0}, encode_outputs{0}, encode_errors{0};
  std::atomic<uint64_t> encoded_bytes{0}, encoded_keys{0}, encoder_drops{0};
  std::atomic<uint64_t> software_identity_events{0}, encoder_creations{0};
  std::atomic<bool> builtin_encoder_format_verified{false};
  std::atomic<uint64_t> decoder_inputs{0}, decoder_outputs{0}, decode_errors{0};
  std::atomic<uint64_t> rendered_frames{0}, nonblack_frames{0};
  std::atomic<uint64_t> invalid_frames{0}, disconnected_events{0};
  std::atomic<uint64_t> encoder_pending{0}, encoder_pending_peak{0};
  std::atomic<uint64_t> encoder_release_calls{0}, encoder_release_errors{0};
  std::atomic<uint64_t> encoder_pending_after_release{0};
  std::atomic<uint64_t> decoder_pending{0}, decoder_pending_peak{0};
  std::atomic<uint64_t> decoder_release_calls{0}, decoder_release_errors{0};
  std::atomic<uint64_t> decoder_pending_after_release{0};
  std::atomic<uint32_t> first_encoded_timestamp{0}, last_encoded_timestamp{0};
  std::atomic<uint32_t> first_decoded_timestamp{0}, last_decoded_timestamp{0};
  LatencyHistogram encode_latency, key_encode_latency, delta_encode_latency;
  LatencyHistogram encode_to_callback_latency, callback_latency;
  LatencyHistogram desktop_capture_latency, desktop_conversion_latency;
};

bool WaitFor(const std::shared_ptr<Shared>& state,
             const std::function<bool()>& predicate,
             int seconds,
             const char* reason) {
  const auto deadline = SteadyClock::now() + std::chrono::seconds(seconds);
  while (!state->failed.load() && SteadyClock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!state->failed.load())
    state->Fail(reason);
  return false;
}

SdpVideoFormat Format(const Options& options) {
  if (options.codec == "H265") {
    return SdpVideoFormat("H265",
                          {{"profile-id", "1"},
                           {"tier-flag", "0"},
                           {"level-id", options.width == 1920 ? "120" : "93"},
                           {"tx-mode", "SRST"}});
  }
  return SdpVideoFormat("H264", {{"level-asymmetry-allowed", "1"},
                                 {"packetization-mode", "1"},
                                 {"profile-level-id", "420028"}});
}

class VerifiedEncoder final : public VideoEncoder, public EncodedImageCallback {
 public:
  VerifiedEncoder(std::unique_ptr<VideoEncoder> inner,
                  std::shared_ptr<Shared> state)
      : inner_(std::move(inner)), state_(std::move(state)) {
    ++state_->encoder_creations;
  }
  int InitEncode(const VideoCodec* codec, const Settings& settings) override {
    if (!codec || codec->width != state_->options.width ||
        codec->height != state_->options.height) {
      state_->Fail(
          "negotiated encoder dimensions differ from requested dimensions");
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    const int result = inner_->InitEncode(codec, settings);
    if (result != WEBRTC_VIDEO_CODEC_OK || !CheckIdentity()) {
      ++state_->encode_errors;
      state_->Fail(state_->options.backend +
                   " encoder InitEncode failed or changed hardware identity");
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    return result;
  }
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override {
    callback_ = callback;
    return inner_->RegisterEncodeCompleteCallback(this);
  }
  int32_t Release() override {
    const int32_t result = inner_->Release();
    ++state_->encoder_release_calls;
    UpdatePeak(state_->encoder_pending_after_release,
               state_->encoder_pending.load());
    if (result != WEBRTC_VIDEO_CODEC_OK || active_ ||
        state_->encoder_pending != 0) {
      ++state_->encoder_release_errors;
      state_->Fail("encoder Release failed or left an active Encode call");
    }
    callback_ = nullptr;
    return result;
  }
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* types) override {
    const auto start = SteadyClock::now();
    ++state_->encode_calls;
    if (!CheckIdentity()) {
      state_->encode_latency.Add(
          Milliseconds(SteadyClock::now() - start).count());
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    const uint64_t pending = state_->encoder_pending.fetch_add(1) + 1;
    UpdatePeak(state_->encoder_pending_peak, pending);
    if (active_ || pending != 1) {
      --state_->encoder_pending;
      ++state_->encode_errors;
      state_->Fail("encoder received overlapping synchronous Encode calls");
      state_->encode_latency.Add(
          Milliseconds(SteadyClock::now() - start).count());
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    expected_timestamp_ = frame.rtp_timestamp();
    callbacks_ = 0;
    emitted_key_ = false;
    encode_start_ = start;
    active_ = true;
    const int32_t result = inner_->Encode(frame, types);
    active_ = false;
    --state_->encoder_pending;
    const bool empty = types && !types->empty() &&
                       types->front() == VideoFrameType::kEmptyFrame;
    const bool valid = result == WEBRTC_VIDEO_CODEC_OK &&
                       (empty || callbacks_ == 1) && CheckIdentity();
    const double complete_ms = Milliseconds(SteadyClock::now() - start).count();
    state_->encode_latency.Add(complete_ms);
    if (!empty && callbacks_ == 1) {
      if (emitted_key_)
        state_->key_encode_latency.Add(complete_ms);
      else
        state_->delta_encode_latency.Add(complete_ms);
    }
    if (!valid) {
      ++state_->encode_errors;
      state_->Fail(state_->options.backend +
                   " Encode failed, omitted output, or violated synchronous "
                   "one-AU contract");
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    return result;
  }
  void SetRates(const RateControlParameters& rates) override {
    inner_->SetRates(rates);
    CheckIdentity();
  }
  void SetFecControllerOverride(FecControllerOverride* controller) override {
    inner_->SetFecControllerOverride(controller);
  }
  void OnPacketLossRateUpdate(float rate) override {
    inner_->OnPacketLossRateUpdate(rate);
  }
  void OnRttUpdate(int64_t rtt) override { inner_->OnRttUpdate(rtt); }
  void OnLossNotification(const LossNotification& notification) override {
    inner_->OnLossNotification(notification);
  }
  EncoderInfo GetEncoderInfo() const override {
    return inner_->GetEncoderInfo();
  }
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* info) override {
    const auto callback_start = SteadyClock::now();
    ++callbacks_;
    const bool valid =
        active_ && callbacks_ == 1 && callback_ &&
        image.RtpTimestamp() == expected_timestamp_ && image.data() &&
        image.size() > 0 &&
        image._encodedWidth == static_cast<uint32_t>(state_->options.width) &&
        image._encodedHeight == static_cast<uint32_t>(state_->options.height) &&
        info &&
        info->codecType == (state_->options.codec == "H264" ? kVideoCodecH264
                                                            : kVideoCodecH265);
    if (!valid) {
      ++state_->encode_errors;
      state_->Fail(
          "encoded callback metadata, size, timing, or frame ID was invalid");
      return Result(Result::ERROR_SEND_FAILED);
    }
    state_->encode_to_callback_latency.Add(
        Milliseconds(callback_start - encode_start_).count());
    if (state_->encode_outputs.fetch_add(1) == 0)
      state_->first_encoded_timestamp = image.RtpTimestamp();
    state_->last_encoded_timestamp = image.RtpTimestamp();
    state_->encoded_bytes += image.size();
    emitted_key_ = image.FrameType() == VideoFrameType::kVideoFrameKey;
    if (emitted_key_) {
      ++state_->encoded_keys;
      // Keep at most two key access units; never retain a long raw sequence.
      if (image.size() <= 8 * 1024 * 1024) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->last_key.assign(image.data(), image.data() + image.size());
        if (state_->first_key.empty())
          state_->first_key = state_->last_key;
      }
    }
    const Result result = callback_->OnEncodedImage(image, info);
    state_->callback_latency.Add(
        Milliseconds(SteadyClock::now() - callback_start).count());
    return result;
  }
  void OnDroppedFrame(DropReason reason) override {
    ++state_->encoder_drops;
    if (callback_)
      callback_->OnDroppedFrame(reason);
  }

 private:
  bool CheckIdentity() {
    const auto identity = inner_->GetEncoderInfo();
    state_->SetEncoderName(identity.implementation_name);
    if (!identity.is_hardware_accelerated ||
        !identity.implementation_name.starts_with(
            state_->options.backend + " " + state_->options.codec)) {
      ++state_->software_identity_events;
      state_->Fail(
          "encoder identity is not the requested " + state_->options.backend +
          " " + state_->options.codec +
          " hardware implementation; actual=" + identity.implementation_name);
      return false;
    }
    return true;
  }
  std::unique_ptr<VideoEncoder> inner_;
  std::shared_ptr<Shared> state_;
  EncodedImageCallback* callback_ = nullptr;
  uint32_t expected_timestamp_ = 0;
  int callbacks_ = 0;
  bool active_ = false, emitted_key_ = false;
  SteadyClock::time_point encode_start_;
};

class VerifiedEncoderFactory final : public VideoEncoderFactory {
 public:
  explicit VerifiedEncoderFactory(std::shared_ptr<Shared> state)
      : inner_(CreateBuiltinVideoEncoderFactory()), state_(std::move(state)) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    const auto requested = Format(state_->options);
    const auto available = inner_->GetSupportedFormats();
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->builtin_encoder_formats.clear();
      for (const auto& format : available)
        state_->builtin_encoder_formats.push_back(format.ToString());
    }
    const bool covered =
        NativeCapabilityCovers(requested, available) &&
        inner_->QueryCodecSupport(requested, std::nullopt).is_supported;
    state_->builtin_encoder_format_verified = covered;
    if (!covered) {
      state_->Fail(
          "builtin advertised encoder capabilities do not cover the requested "
          "profile and level");
      return {};
    }
    return {requested};
  }
  CodecSupport QueryCodecSupport(
      const SdpVideoFormat& format,
      std::optional<std::string> scalability) const override {
    if (!NativeCapabilityCovers(format, inner_->GetSupportedFormats()))
      return {};
    return inner_->QueryCodecSupport(format, std::move(scalability));
  }
  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override {
    if (format.name != state_->options.codec) {
      state_->Fail("unexpected encoder codec was requested");
      return nullptr;
    }
    auto encoder = inner_->Create(env, format);
    if (!encoder) {
      state_->Fail("builtin factory could not create forced " +
                   state_->options.backend + " " + state_->options.codec +
                   " encoder");
      return nullptr;
    }
    return std::make_unique<VerifiedEncoder>(std::move(encoder), state_);
  }

 private:
  bool NativeCapabilityCovers(
      const SdpVideoFormat& requested,
      const std::vector<SdpVideoFormat>& available) const {
    if (requested.name != state_->options.codec)
      return false;
    for (const auto& candidate : available) {
      if (requested.name == "H264" && IsBackendH264FormatSupported(requested) &&
          IsBackendH264FormatSupported(candidate)) {
        const auto expected =
            ParseSdpForH264ProfileLevelId(requested.parameters);
        const auto actual = ParseSdpForH264ProfileLevelId(candidate.parameters);
        if (expected && actual && expected->profile == actual->profile &&
            static_cast<int>(actual->level) >=
                static_cast<int>(expected->level))
          return true;
      } else if (requested.name == "H265" &&
                 IsBackendH265FormatSupported(requested) &&
                 IsBackendH265FormatSupported(candidate)) {
        const auto expected =
            ParseSdpForH265ProfileTierLevel(requested.parameters);
        const auto actual =
            ParseSdpForH265ProfileTierLevel(candidate.parameters);
        if (expected && actual && expected->profile == actual->profile &&
            expected->tier == actual->tier &&
            static_cast<int>(actual->level) >=
                static_cast<int>(expected->level))
          return true;
      }
    }
    return false;
  }
  std::unique_ptr<VideoEncoderFactory> inner_;
  std::shared_ptr<Shared> state_;
};

class VerifiedDecoder final : public VideoDecoder, public DecodedImageCallback {
 public:
  VerifiedDecoder(std::unique_ptr<VideoDecoder> inner,
                  std::shared_ptr<Shared> state,
                  FfmpegH265TestDecoder* reference = nullptr)
      : inner_(std::move(inner)),
        state_(std::move(state)),
        reference_(reference) {}
  bool Configure(const Settings& settings) override {
    const bool configured = inner_->Configure(settings);
    const auto identity = inner_->GetDecoderInfo();
    state_->SetDecoderName(identity.implementation_name);
    if (!configured) {
      ++state_->decode_errors;
      state_->Fail(Diagnostic("receiver decoder Configure failed"));
    } else if (reference_ && (identity.is_hardware_accelerated ||
                              !identity.implementation_name.starts_with(
                                  "FFmpeg HEVC test software"))) {
      ++state_->decode_errors;
      state_->Fail(
          "HEVC receiver is not the declared software reference decoder");
      return false;
    }
    return configured;
  }
  int32_t Decode(const EncodedImage& image, int64_t render_ms) override {
    ++state_->decoder_inputs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_.insert(image.RtpTimestamp()).second) {
        ++state_->decode_errors;
        state_->Fail("decoder received a duplicate pending RTP frame ID");
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      const uint64_t pending = state_->decoder_pending.fetch_add(1) + 1;
      UpdatePeak(state_->decoder_pending_peak, pending);
      if (pending_.size() > 256) {
        ++state_->decode_errors;
        state_->Fail("receiver decoder has excessive uncompleted frame IDs");
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
    }
    const int32_t result = inner_->Decode(image, render_ms);
    if (result < 0) {
      ++state_->decode_errors;
      state_->Fail(Diagnostic("receiver decoder Decode returned an error"));
    }
    return result;
  }
  int32_t RegisterDecodeCompleteCallback(
      DecodedImageCallback* callback) override {
    callback_ = callback;
    return inner_->RegisterDecodeCompleteCallback(this);
  }
  int32_t Release() override {
    const int32_t result = inner_->Release();
    ++state_->decoder_release_calls;
    size_t pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending = pending_.size();
    }
    UpdatePeak(state_->decoder_pending_after_release, pending);
    if (result != WEBRTC_VIDEO_CODEC_OK || pending != 0) {
      ++state_->decoder_release_errors;
      ++state_->decode_errors;
      state_->Fail(Diagnostic(
          "receiver decoder Release failed or left pending RTP frame IDs"));
    }
    callback_ = nullptr;
    return result;
  }
  DecoderInfo GetDecoderInfo() const override {
    return inner_->GetDecoderInfo();
  }
  int32_t Decoded(VideoFrame& frame) override {
    return Deliver(frame, std::nullopt, std::nullopt);
  }
  int32_t Decoded(VideoFrame& frame, int64_t decode_ms) override {
    return Deliver(frame, static_cast<int32_t>(decode_ms), std::nullopt);
  }
  void Decoded(VideoFrame& frame,
               std::optional<int32_t> decode_ms,
               std::optional<uint8_t> qp) override {
    Deliver(frame, decode_ms, qp);
  }

 private:
  int32_t Deliver(VideoFrame& frame,
                  std::optional<int32_t> decode_ms,
                  std::optional<uint8_t> qp) {
    bool known;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      known = pending_.erase(frame.rtp_timestamp()) == 1;
      if (known)
        --state_->decoder_pending;
    }
    if (!known || !callback_) {
      ++state_->decode_errors;
      state_->Fail("decoded callback does not match a submitted RTP frame ID");
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (state_->decoder_outputs.fetch_add(1) == 0)
      state_->first_decoded_timestamp = frame.rtp_timestamp();
    state_->last_decoded_timestamp = frame.rtp_timestamp();
    callback_->Decoded(frame, decode_ms, qp);
    return WEBRTC_VIDEO_CODEC_OK;
  }
  std::string Diagnostic(const char* operation) const {
    return reference_ ? std::string(operation) + ": " + reference_->last_error()
                      : operation;
  }
  std::unique_ptr<VideoDecoder> inner_;
  std::shared_ptr<Shared> state_;
  FfmpegH265TestDecoder* const reference_;
  DecodedImageCallback* callback_ = nullptr;
  std::mutex mutex_;
  std::unordered_set<uint32_t> pending_;
};

class VerifiedDecoderFactory final : public VideoDecoderFactory {
 public:
  explicit VerifiedDecoderFactory(std::shared_ptr<Shared> state)
      : inner_(CreateBuiltinVideoDecoderFactory()), state_(std::move(state)) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    const auto format = Format(state_->options);
    return QueryCodecSupport(format, false).is_supported
               ? std::vector<SdpVideoFormat>{format}
               : std::vector<SdpVideoFormat>{};
  }
  CodecSupport QueryCodecSupport(const SdpVideoFormat& format,
                                 bool scaling) const override {
    if (state_->options.codec == "H265") {
      const auto expected = Format(state_->options);
      const auto directory =
          std::filesystem::path(state_->options.ffmpeg_dll_directory);
      // Advertise only the declared Main / main-tier / SRST format. Presence
      // is checked here; Configure enforces exact DLL versions and symbols.
      bool parameters_match = format.name == expected.name;
      for (const auto& [key, value] : expected.parameters) {
        const auto found = format.parameters.find(key);
        parameters_match &=
            found != format.parameters.end() && found->second == value;
      }
      return {parameters_match && !scaling && directory.is_absolute() &&
                  FileExists(directory / "avcodec-61.dll") &&
                  FileExists(directory / "avutil-59.dll"),
              false};
    }
    if (format.name != state_->options.codec)
      return {};
    return inner_->QueryCodecSupport(format, scaling);
  }
  std::unique_ptr<VideoDecoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override {
    if (!QueryCodecSupport(format, false).is_supported) {
      state_->Fail(
          "receiver codec format or HEVC reference DLLs are unsupported");
      return nullptr;
    }
    if (state_->options.codec == "H265") {
      auto decoder = std::make_unique<FfmpegH265TestDecoder>(
          state_->options.ffmpeg_dll_directory);
      auto* reference = decoder.get();
      return std::make_unique<VerifiedDecoder>(std::move(decoder), state_,
                                               reference);
    }
    auto decoder = inner_->Create(env, format);
    if (!decoder) {
      state_->Fail("native builtin decoder creation failed");
      return nullptr;
    }
    return std::make_unique<VerifiedDecoder>(std::move(decoder), state_);
  }

 private:
  std::unique_ptr<VideoDecoderFactory> inner_;
  std::shared_ptr<Shared> state_;
};

class SyntheticSource : public VideoTrackSource {
 public:
  SyntheticSource() : VideoTrackSource(false) { SetState(kLive); }
  void Push(const VideoFrame& frame) { broadcaster_.OnFrame(frame); }
  void End() { SetState(kEnded); }

 protected:
  ~SyntheticSource() override = default;

 private:
  VideoSourceInterface<VideoFrame>* source() override { return &broadcaster_; }
  VideoBroadcaster broadcaster_;
};

std::vector<scoped_refptr<I420Buffer>> MakeFrames(const Options& options) {
  std::vector<scoped_refptr<I420Buffer>> frames;
  for (int index = 0; index < 60; ++index) {
    auto frame = I420Buffer::Create(options.width, options.height);
    const int square_x = (index * 13) % (options.width - 80);
    const int square_y = (index * 7) % (options.height - 80);
    for (int y = 0; y < options.height; ++y) {
      for (int x = 0; x < options.width; ++x) {
        const bool square = x >= square_x && x < square_x + 80 &&
                            y >= square_y && y < square_y + 80;
        frame->MutableDataY()[y * frame->StrideY() + x] =
            static_cast<uint8_t>(square ? 220 : 32 + ((x / 8 + y / 8) % 128));
      }
    }
    for (int y = 0; y < options.height / 2; ++y) {
      for (int x = 0; x < options.width / 2; ++x) {
        frame->MutableDataU()[y * frame->StrideU() + x] =
            static_cast<uint8_t>(72 + (x / 8) % 32);
        frame->MutableDataV()[y * frame->StrideV() + x] =
            static_cast<uint8_t>(160 + (y / 8) % 32);
      }
    }
    frames.push_back(std::move(frame));
  }
  return frames;
}

std::string Utf8(const wchar_t* value) {
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0)
    return {};
  std::string result(static_cast<size_t>(size), '\0');
  if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr,
                           nullptr))
    return {};
  result.resize(static_cast<size_t>(size - 1));
  return result;
}

bool FindPrimary(DesktopCapturer& capturer,
                 DesktopCapturer::SourceId* source,
                 std::string* device_name,
                 RECT* monitor_rect) {
  const POINT origin = {0, 0};
  const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor ||
      !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) ||
      !(info.dwFlags & MONITORINFOF_PRIMARY))
    return false;
  DesktopCapturer::SourceList sources;
  if (!capturer.GetSourceList(&sources))
    return false;
  for (const auto& entry : sources) {
    if (entry.id < 0 || static_cast<uint64_t>(entry.id) > MAXDWORD)
      continue;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(nullptr, static_cast<DWORD>(entry.id), &device,
                            0) &&
        (device.StateFlags & DISPLAY_DEVICE_ACTIVE) &&
        (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) &&
        std::wcscmp(device.DeviceName, info.szDevice) == 0) {
      *source = entry.id;
      *device_name = Utf8(device.DeviceName);
      *monitor_rect = info.rcMonitor;
      return capturer.SelectSource(entry.id);
    }
  }
  return false;
}

// Created, captured, and destroyed only on Run's main owner thread. Only the
// current desktop pixels and converted I420 frame are retained; there are no
// capture tasks, queues, or on-disk raw desktop recordings.
class PrimaryDesktopInput final : public DesktopCapturer::Callback {
 public:
  explicit PrimaryDesktopInput(std::shared_ptr<Shared> state)
      : state_(std::move(state)), owner_thread_(GetCurrentThreadId()) {}
  ~PrimaryDesktopInput() override { Stop(); }
  bool Initialize() {
    if (!CheckOwner())
      return false;
    auto options = DesktopCaptureOptions::CreateDefault();
    options.set_allow_directx_capturer(false);
    options.set_disable_effects(false);
    options.set_detect_updated_region(false);
    options.set_prefer_cursor_embedded(false);
#if defined(RTC_ENABLE_WIN_WGC)
    options.set_allow_wgc_screen_capturer(false);
#endif
    capturer_ = DesktopCapturer::CreateScreenCapturer(options);
    if (!capturer_ || !FindPrimary(*capturer_, &source_before_, &device_before_,
                                   &rect_before_)) {
      ++errors_;
      state_->Fail("primary GDI source could not be selected reliably");
      return false;
    }
    initialized_ = true;
    capturer_->Start(this);
    return true;
  }
  scoped_refptr<I420Buffer> Capture() {
    if (!CheckOwner() || !initialized_ || !capturer_)
      return nullptr;
    callbacks_ = 0;
    capture_result_ = DesktopCapturer::Result::ERROR_PERMANENT;
    frame_.reset();
    ++attempts_;
    const auto capture_start = SteadyClock::now();
    capturer_->CaptureFrame();
    state_->desktop_capture_latency.Add(
        Milliseconds(SteadyClock::now() - capture_start).count());
    if (callbacks_ != 1 ||
        capture_result_ != DesktopCapturer::Result::SUCCESS || !frame_) {
      ++errors_;
      state_->Fail(
          "GDI capture failed or did not synchronously return one frame");
      frame_.reset();
      return nullptr;
    }
    last_capturer_id_ = frame_->capturer_id();
    last_width_ = frame_->size().width();
    last_height_ = frame_->size().height();
    if (attempts_ == 1) {
      first_capturer_id_ = last_capturer_id_;
      first_width_ = last_width_;
      first_height_ = last_height_;
    }
    if (last_capturer_id_ != DesktopCapturerId::kScreenCapturerWinGdi ||
        first_width_ != last_width_ || first_height_ != last_height_) {
      ++errors_;
      state_->Fail(
          "desktop capturer identity or primary screen dimensions changed");
      frame_.reset();
      return nullptr;
    }
    const auto conversion_start = SteadyClock::now();
    auto converted = Convert(*frame_);
    state_->desktop_conversion_latency.Add(
        Milliseconds(SteadyClock::now() - conversion_start).count());
    frame_.reset();
    if (!converted) {
      ++errors_;
      state_->Fail("desktop ARGB-to-I420 conversion or fitted scaling failed");
    }
    return converted;
  }
  void Stop() {
    if (!capturer_)
      return;
    if (!CheckOwner())
      return;
    if (initialized_) {
      if (!FindPrimary(*capturer_, &source_after_, &device_after_,
                       &rect_after_) ||
          source_before_ != source_after_ || device_before_ != device_after_ ||
          !EqualRect(&rect_before_, &rect_after_)) {
        ++errors_;
        state_->Fail(
            "primary screen source identity or bounds changed during capture");
      }
    }
    capturer_.reset();
    frame_.reset();
    destroyed_on_owner_ = true;
  }
  void OnCaptureResult(DesktopCapturer::Result result,
                       std::unique_ptr<DesktopFrame> frame) override {
    if (!CheckOwner())
      return;
    ++callbacks_;
    ++total_callbacks_;
    capture_result_ = result;
    frame_ = std::move(frame);
  }
  std::string Json() const {
    std::ostringstream out;
    out << "{\"initialized\":" << (initialized_ ? "true" : "false")
        << ",\"owner_thread_id\":" << owner_thread_
        << ",\"owner_thread_violation\":"
        << (owner_violation_ ? "true" : "false")
        << ",\"capturer_destroyed_on_owner_thread\":"
        << (destroyed_on_owner_ ? "true" : "false")
        << ",\"capture_attempts\":" << attempts_
        << ",\"capture_callbacks\":" << total_callbacks_
        << ",\"capture_errors\":" << errors_
        << ",\"last_capture_result\":" << static_cast<int>(capture_result_)
        << ",\"source_id_before\":" << source_before_
        << ",\"source_id_after\":" << source_after_
        << ",\"device_name_before\":" << Quote(device_before_)
        << ",\"device_name_after\":" << Quote(device_after_)
        << ",\"first_capturer_id\":" << first_capturer_id_
        << ",\"last_capturer_id\":" << last_capturer_id_
        << ",\"source_width_before\":" << first_width_
        << ",\"source_height_before\":" << first_height_
        << ",\"source_width_after\":" << last_width_
        << ",\"source_height_after\":" << last_height_
        << ",\"content_width\":" << content_width_
        << ",\"content_height\":" << content_height_
        << ",\"content_x\":" << content_x_ << ",\"content_y\":" << content_y_
        << ",\"capture_latency\":" << state_->desktop_capture_latency.Json()
        << ",\"conversion_latency\":"
        << state_->desktop_conversion_latency.Json()
        << ",\"retention\":\"latest frame only; no raw desktop sequence "
           "saved\"}";
    return out.str();
  }

 private:
  bool CheckOwner() {
    if (GetCurrentThreadId() == owner_thread_)
      return true;
    owner_violation_ = true;
    state_->Fail(
        "desktop capturer operation ran outside its main owner thread");
    return false;
  }
  scoped_refptr<I420Buffer> Convert(const DesktopFrame& frame) {
    const int width = frame.size().width(), height = frame.size().height();
    if (!frame.data() || width < 2 || height < 2 || width > 16384 ||
        height > 16384 || frame.stride() < width * 4)
      return nullptr;
    auto native = I420Buffer::Create(width, height);
    if (libyuv::ARGBToI420(frame.data(), frame.stride(), native->MutableDataY(),
                           native->StrideY(), native->MutableDataU(),
                           native->StrideU(), native->MutableDataV(),
                           native->StrideV(), width, height) != 0)
      return nullptr;
    const int output_width = state_->options.width;
    const int output_height = state_->options.height;
    content_width_ = output_width;
    content_height_ = output_height;
    if (static_cast<int64_t>(width) * output_height >
        static_cast<int64_t>(height) * output_width)
      content_height_ = static_cast<int>(static_cast<int64_t>(height) *
                                         output_width / width) &
                        ~1;
    else
      content_width_ = static_cast<int>(static_cast<int64_t>(width) *
                                        output_height / height) &
                       ~1;
    if (content_width_ < 2 || content_height_ < 2)
      return nullptr;
    content_x_ = ((output_width - content_width_) / 2) & ~1;
    content_y_ = ((output_height - content_height_) / 2) & ~1;
    auto output = I420Buffer::Create(output_width, output_height);
    I420Buffer::SetBlack(output.get());
    if (libyuv::I420Scale(
            native->DataY(), native->StrideY(), native->DataU(),
            native->StrideU(), native->DataV(), native->StrideV(), width,
            height,
            output->MutableDataY() + content_y_ * output->StrideY() +
                content_x_,
            output->StrideY(),
            output->MutableDataU() + (content_y_ / 2) * output->StrideU() +
                content_x_ / 2,
            output->StrideU(),
            output->MutableDataV() + (content_y_ / 2) * output->StrideV() +
                content_x_ / 2,
            output->StrideV(), content_width_, content_height_,
            libyuv::kFilterBox) != 0)
      return nullptr;
    return output;
  }
  std::shared_ptr<Shared> state_;
  const DWORD owner_thread_;
  bool initialized_ = false, destroyed_on_owner_ = false;
  std::atomic<bool> owner_violation_{false};
  std::unique_ptr<DesktopCapturer> capturer_;
  std::unique_ptr<DesktopFrame> frame_;
  DesktopCapturer::Result capture_result_ =
      DesktopCapturer::Result::ERROR_PERMANENT;
  DesktopCapturer::SourceId source_before_ = -2, source_after_ = -2;
  std::string device_before_, device_after_;
  RECT rect_before_{}, rect_after_{};
  int callbacks_ = 0, first_width_ = 0, first_height_ = 0;
  int last_width_ = 0, last_height_ = 0;
  int content_width_ = 0, content_height_ = 0, content_x_ = 0, content_y_ = 0;
  uint32_t first_capturer_id_ = 0, last_capturer_id_ = 0;
  uint64_t attempts_ = 0, total_callbacks_ = 0, errors_ = 0;
};

class FrameSink final : public VideoSinkInterface<VideoFrame> {
 public:
  explicit FrameSink(std::shared_ptr<Shared> state)
      : state_(std::move(state)) {}
  void OnFrame(const VideoFrame& frame) override {
    ++state_->rendered_frames;
    if (frame.width() != state_->options.width ||
        frame.height() != state_->options.height ||
        !frame.video_frame_buffer()) {
      ++state_->invalid_frames;
      state_->Fail(
          "received decoded frame has invalid dimensions or no buffer");
      return;
    }
    const auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) {
      ++state_->invalid_frames;
      state_->Fail("received frame cannot expose decoded I420 pixels");
      return;
    }
    int minimum = 255, maximum = 0;
    for (int y = 0; y < i420->height(); y += 23) {
      for (int x = 0; x < i420->width(); x += 29) {
        const int value = i420->DataY()[y * i420->StrideY() + x];
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
      }
    }
    if (maximum - minimum >= 30 && maximum > 40)
      ++state_->nonblack_frames;
    else {
      ++state_->invalid_frames;
      state_->Fail(
          "received decoded scene is black or lacks required luma variation");
    }
  }

 private:
  std::shared_ptr<Shared> state_;
};

class Observer final : public PeerConnectionObserver {
 public:
  Observer(std::shared_ptr<Shared> state, FrameSink* sink)
      : state_(std::move(state)), sink_(sink) {}
  void OnSignalingChange(PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(scoped_refptr<DataChannelInterface>) override {
    state_->Fail("unexpected data channel in video-only smoke");
  }
  void OnIceGatheringChange(
      PeerConnectionInterface::IceGatheringState value) override {
    state_->gathered = value == PeerConnectionInterface::kIceGatheringComplete;
  }
  void OnIceCandidate(const IceCandidate*) override { ++state_->candidates; }
  void OnIceConnectionChange(
      PeerConnectionInterface::IceConnectionState value) override {
    state_->ice_state = static_cast<int>(value);
    if (value == PeerConnectionInterface::kIceConnectionFailed &&
        !state_->closing)
      state_->Fail("ICE connection failed");
  }
  void OnConnectionChange(
      PeerConnectionInterface::PeerConnectionState value) override {
    state_->connection_state = static_cast<int>(value);
    if (value == PeerConnectionInterface::PeerConnectionState::kConnected)
      state_->connected_once = true;
    if (value == PeerConnectionInterface::PeerConnectionState::kDisconnected &&
        state_->connected_once && !state_->closing) {
      ++state_->disconnected_events;
      state_->Fail("PeerConnection disconnected during active media interval");
    }
    if (value == PeerConnectionInterface::PeerConnectionState::kFailed &&
        !state_->closing)
      state_->Fail("PeerConnection ICE/DTLS aggregate failed");
  }
  void OnTrack(scoped_refptr<RtpTransceiverInterface> transceiver) override {
    if (state_->options.role != "receiver") {
      state_->Fail("sender unexpectedly received a remote track");
      return;
    }
    auto track = transceiver->receiver()->track();
    if (!track || track->kind() != MediaStreamTrackInterface::kVideoKind) {
      state_->Fail("receiver got a non-video or missing track");
      return;
    }
    auto video = scoped_refptr<VideoTrackInterface>(
        static_cast<VideoTrackInterface*>(track.get()));
    video->AddOrUpdateSink(sink_, VideoSinkWants());
    tracks_.push_back(std::move(video));
  }
  void Detach() {
    for (auto& track : tracks_)
      track->RemoveSink(sink_);
    tracks_.clear();
  }

 private:
  std::shared_ptr<Shared> state_;
  FrameSink* sink_;
  std::vector<scoped_refptr<VideoTrackInterface>> tracks_;
};

class LocalSetObserver : public SetLocalDescriptionObserverInterface {
 public:
  explicit LocalSetObserver(std::shared_ptr<Shared> state)
      : state_(std::move(state)) {}
  void OnSetLocalDescriptionComplete(RTCError error) override {
    if (error.ok())
      state_->local_set = true;
    else
      state_->Fail("SetLocalDescription: " + std::string(error.message()));
  }

 private:
  std::shared_ptr<Shared> state_;
};

class RemoteSetObserver : public SetRemoteDescriptionObserverInterface {
 public:
  explicit RemoteSetObserver(std::shared_ptr<Shared> state)
      : state_(std::move(state)) {}
  void OnSetRemoteDescriptionComplete(RTCError error) override {
    if (error.ok())
      state_->remote_set = true;
    else
      state_->Fail("SetRemoteDescription: " + std::string(error.message()));
  }

 private:
  std::shared_ptr<Shared> state_;
};

class DescriptionObserver : public CreateSessionDescriptionObserver {
 public:
  DescriptionObserver(scoped_refptr<PeerConnectionInterface> pc,
                      std::shared_ptr<Shared> state)
      : pc_(std::move(pc)), state_(std::move(state)) {}
  void OnSuccess(SessionDescriptionInterface* description) override {
    pc_->SetLocalDescription(
        std::unique_ptr<SessionDescriptionInterface>(description),
        make_ref_counted<LocalSetObserver>(state_));
  }
  void OnFailure(RTCError error) override {
    state_->Fail("CreateOffer/CreateAnswer: " + std::string(error.message()));
  }

 private:
  scoped_refptr<PeerConnectionInterface> pc_;
  std::shared_ptr<Shared> state_;
};

struct Stats {
  uint64_t bytes_sent = 0, bytes_received = 0;
  uint64_t frames_encoded = 0, frames_decoded = 0, packets_lost = 0;
  bool dtls_connected = false, srtp = false, udp_selected = false;
  bool codec_verified = false;
  std::string json, mime_type, dtls_cipher, srtp_cipher;
};

class StatsObserver : public RTCStatsCollectorCallback {
 public:
  explicit StatsObserver(std::shared_ptr<Shared> state)
      : state_(std::move(state)) {}
  void OnStatsDelivered(
      const scoped_refptr<const RTCStatsReport>& report) override {
    Stats snapshot;
    snapshot.json = report->ToJson();
    for (const auto* transport : report->GetStatsOfType<RTCTransportStats>()) {
      snapshot.dtls_connected |= transport->dtls_state == "connected";
      snapshot.srtp |= transport->srtp_cipher.has_value() &&
                       !transport->srtp_cipher->empty();
      if (transport->dtls_cipher)
        snapshot.dtls_cipher = *transport->dtls_cipher;
      if (transport->srtp_cipher)
        snapshot.srtp_cipher = *transport->srtp_cipher;
      if (transport->selected_candidate_pair_id) {
        const auto* pair = report->GetAs<RTCIceCandidatePairStats>(
            *transport->selected_candidate_pair_id);
        if (pair && pair->state == "succeeded" && pair->local_candidate_id) {
          const auto* candidate = report->GetAs<RTCLocalIceCandidateStats>(
              *pair->local_candidate_id);
          snapshot.udp_selected |= candidate && candidate->protocol == "udp";
        }
      }
    }
    for (const auto* outbound :
         report->GetStatsOfType<RTCOutboundRtpStreamStats>()) {
      if (outbound->kind != "video")
        continue;
      snapshot.bytes_sent += outbound->bytes_sent.value_or(0);
      snapshot.frames_encoded += outbound->frames_encoded.value_or(0);
      if (outbound->codec_id)
        CheckCodec(*report, *outbound->codec_id, &snapshot);
    }
    for (const auto* inbound :
         report->GetStatsOfType<RTCInboundRtpStreamStats>()) {
      if (inbound->kind != "video")
        continue;
      snapshot.bytes_received += inbound->bytes_received.value_or(0);
      snapshot.frames_decoded += inbound->frames_decoded.value_or(0);
      snapshot.packets_lost += static_cast<uint64_t>(
          std::max<int64_t>(0, inbound->packets_lost.value_or(0)));
      if (inbound->codec_id)
        CheckCodec(*report, *inbound->codec_id, &snapshot);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot_ = std::move(snapshot);
      ready_ = true;
    }
    condition_.notify_one();
  }
  bool Await(Stats* result) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(5),
                             [this] { return ready_; })) {
      state_->Fail("GetStats delivery timed out");
      return false;
    }
    *result = std::move(snapshot_);
    return true;
  }

 private:
  void CheckCodec(const RTCStatsReport& report,
                  const std::string& id,
                  Stats* stats) {
    const auto* codec = report.GetAs<RTCCodecStats>(id);
    if (codec && codec->mime_type) {
      stats->mime_type = *codec->mime_type;
      stats->codec_verified |=
          *codec->mime_type == "video/" + state_->options.codec;
      if (*codec->mime_type != "video/" + state_->options.codec)
        state_->Fail("actual RTP stream negotiated a different codec");
    }
  }
  std::shared_ptr<Shared> state_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool ready_ = false;
  Stats snapshot_;
};

// Remaining orchestration below uses owner-thread barriers for all mutations.
struct Endpoint {
  scoped_refptr<PeerConnectionFactoryInterface> factory;
  scoped_refptr<PeerConnectionInterface> pc;
  scoped_refptr<SyntheticSource> source;
  scoped_refptr<VideoTrackInterface> local_track;
  scoped_refptr<RtpSenderInterface> sender;
};

bool CheckError(const RTCError& error,
                const std::shared_ptr<Shared>& state,
                const char* operation) {
  if (error.ok())
    return true;
  state->Fail(std::string(operation) + ": " + error.message());
  return false;
}

void InitializeEndpoint(Endpoint* endpoint,
                        const std::shared_ptr<Shared>& state,
                        Thread* network,
                        Thread* worker,
                        Thread* signaling,
                        Observer* observer) {
  PeerConnectionFactoryDependencies dependencies;
  dependencies.network_thread = network;
  dependencies.worker_thread = worker;
  dependencies.signaling_thread = signaling;
  dependencies.env = CreateEnvironment(std::make_unique<FieldTrials>(
      "WebRTC-" + state->options.codec + "-HardwareEncoder/" +
      state->options.backend + "/"));
  dependencies.adm = CreateAudioDeviceModule(*dependencies.env,
                                             AudioDeviceModule::kDummyAudio);
  if (!dependencies.adm) {
    state->Fail("dummy audio device creation failed");
    return;
  }
  dependencies.audio_encoder_factory = CreateBuiltinAudioEncoderFactory();
  dependencies.audio_decoder_factory = CreateBuiltinAudioDecoderFactory();
  dependencies.video_encoder_factory =
      std::make_unique<VerifiedEncoderFactory>(state);
  dependencies.video_decoder_factory =
      std::make_unique<VerifiedDecoderFactory>(state);
  EnableMedia(dependencies);
  endpoint->factory =
      CreateModularPeerConnectionFactory(std::move(dependencies));
  if (!endpoint->factory) {
    state->Fail("CreateModularPeerConnectionFactory failed");
    return;
  }
  PeerConnectionFactoryInterface::Options factory_options;
  factory_options.disable_encryption = false;
  factory_options.network_ignore_mask =
      0;  // Allow actual localhost UDP sockets.
  endpoint->factory->SetOptions(factory_options);
  PeerConnectionInterface::RTCConfiguration configuration;
  configuration.sdp_semantics = SdpSemantics::kUnifiedPlan;
  configuration.tcp_candidate_policy =
      PeerConnectionInterface::kTcpCandidatePolicyDisabled;
  configuration.bundle_policy = PeerConnectionInterface::kBundlePolicyMaxBundle;
  // No STUN/TURN service or fake network is used by this local-only harness.
  PeerConnectionDependencies pc_dependencies(observer);
  auto created = endpoint->factory->CreatePeerConnectionOrError(
      configuration, std::move(pc_dependencies));
  if (!created.ok()) {
    state->Fail("CreatePeerConnectionOrError: " +
                std::string(created.error().message()));
    return;
  }
  endpoint->pc = created.MoveValue();
  if (state->options.role != "sender")
    return;
  endpoint->source = make_ref_counted<SyntheticSource>();
  endpoint->local_track = endpoint->factory->CreateVideoTrack(
      endpoint->source,
      state->options.backend + "-" + state->options.source + "-video");
  auto added = endpoint->pc->AddTrack(
      endpoint->local_track, {state->options.backend + "-smoke-stream"});
  if (!added.ok()) {
    state->Fail("AddTrack: " + std::string(added.error().message()));
    return;
  }
  endpoint->sender = added.MoveValue();
  for (const auto& transceiver : endpoint->pc->GetTransceivers()) {
    if (!CheckError(transceiver->SetDirectionWithError(
                        RtpTransceiverDirection::kSendOnly),
                    state, "SetDirection sendonly"))
      return;
    auto capabilities =
        endpoint->factory->GetRtpSenderCapabilities(MediaType::VIDEO);
    std::vector<RtpCodecCapability> codecs;
    for (const auto& codec : capabilities.codecs) {
      if (codec.name == state->options.codec)
        codecs.push_back(codec);
    }
    if (codecs.empty() || !CheckError(transceiver->SetCodecPreferences(codecs),
                                      state, "SetCodecPreferences")) {
      state->Fail("no requested video codec in sender capabilities");
      return;
    }
  }
  BitrateSettings bitrate;
  bitrate.min_bitrate_bps = state->options.bitrate();
  bitrate.start_bitrate_bps = state->options.bitrate();
  bitrate.max_bitrate_bps = state->options.bitrate();
  CheckError(endpoint->pc->SetBitrate(bitrate), state, "SetBitrate");
  auto parameters = endpoint->sender->GetParameters();
  parameters.degradation_preference =
      DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION;
  for (auto& encoding : parameters.encodings) {
    encoding.active = true;
    // The PC budget includes RTP overhead. Let the allocator pass its smaller
    // payload target through instead of clamping the encoder back to the full
    // wire budget while the upstream frame dropper uses the payload target.
    encoding.min_bitrate_bps = 30000;
    encoding.max_bitrate_bps = state->options.bitrate();
    encoding.max_framerate = state->options.fps;
    encoding.scale_resolution_down_by = 1.0;
    encoding.num_temporal_layers = 1;
  }
  CheckError(endpoint->sender->SetParameters(parameters), state,
             "SetParameters");
}

bool GatherAndExport(Endpoint& endpoint,
                     Thread& signaling,
                     const std::shared_ptr<Shared>& state,
                     const std::filesystem::path& file,
                     bool offer) {
  signaling.BlockingCall([&] {
    auto observer = make_ref_counted<DescriptionObserver>(endpoint.pc, state);
    if (offer)
      endpoint.pc->CreateOffer(
          observer.get(), PeerConnectionInterface::RTCOfferAnswerOptions());
    else
      endpoint.pc->CreateAnswer(
          observer.get(), PeerConnectionInterface::RTCOfferAnswerOptions());
  });
  if (!WaitFor(
          state, [&] { return state->local_set && state->gathered; }, 60,
          "local description or complete ICE gathering timed out"))
    return false;
  std::string sdp;
  signaling.BlockingCall([&] {
    if (const auto* description = endpoint.pc->local_description())
      sdp = description->ToString();
  });
  if (sdp.empty() || sdp.find("a=fingerprint:") == std::string::npos ||
      sdp.find("a=candidate:") == std::string::npos ||
      sdp.find(state->options.codec + "/90000") == std::string::npos) {
    state->Fail(
        "fully gathered local SDP lacks codec, ICE candidate or DTLS "
        "fingerprint");
    return false;
  }
  if (!WriteFile(file, sdp)) {
    state->Fail("atomic local SDP write failed");
    return false;
  }
  return true;
}

bool ApplyRemote(Endpoint& endpoint,
                 Thread& signaling,
                 const std::shared_ptr<Shared>& state,
                 const std::filesystem::path& file,
                 SdpType type) {
  if (!WaitFor(
          state, [&] { return FileExists(file); }, 90,
          "remote fully gathered SDP file timed out"))
    return false;
  const std::string sdp = ReadFile(file);
  signaling.BlockingCall([&] {
    SdpParseError error;
    auto description = CreateSessionDescription(type, sdp, &error);
    if (!description) {
      state->Fail("remote SDP parsing failed: " + error.description);
      return;
    }
    endpoint.pc->SetRemoteDescription(
        std::move(description), make_ref_counted<RemoteSetObserver>(state));
  });
  return WaitFor(
      state, [&] { return state->remote_set.load(); }, 15,
      "SetRemoteDescription completion timed out");
}

bool CollectStats(Endpoint& endpoint,
                  Thread& signaling,
                  const std::shared_ptr<Shared>& state,
                  Stats* stats) {
  auto observer = make_ref_counted<StatsObserver>(state);
  signaling.BlockingCall([&] { endpoint.pc->GetStats(observer.get()); });
  return observer->Await(stats);
}

std::string SettingsText(const Options& options) {
  return options.codec + "," + std::to_string(options.width) + "," +
         std::to_string(options.height) + "," + std::to_string(options.fps) +
         "," + std::to_string(options.seconds) + "," + options.source + "," +
         options.backend + "\n";
}

void WriteKeySamples(const std::filesystem::path& directory,
                     const std::shared_ptr<Shared>& state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  for (const auto& named : {std::pair{"first-key", &state->first_key},
                            std::pair{"last-key", &state->last_key}}) {
    if (named.second->empty())
      continue;
    const auto path =
        directory / (state->options.role + "-" + named.first +
                     (state->options.codec == "H264" ? ".h264" : ".h265"));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(named.second->data()),
               static_cast<std::streamsize>(named.second->size()));
    file.close();
    if (!file) {
      // The mutex is already held: record this failure without recursive Fail.
      if (state->failure.empty())
        state->failure = "key sample evidence write failed";
      state->failed = true;
    }
  }
}

// Opt-in diagnostic runs retain only upstream drop/rate messages. Verbose
// message construction changes measurement overhead, so this is disabled for
// formal performance and stability runs. No drop or congestion policy changes.
class DropDiagnostics final : public LogSink {
 public:
  DropDiagnostics(bool enabled, const std::filesystem::path& path)
      : enabled_(enabled) {
    if (!enabled_)
      return;
    if (FileExists(path)) {
      valid_ = false;
      return;
    }
    file_.open(path, std::ios::binary);
    valid_ = file_.good();
    if (valid_) {
      LogMessage::AddLogToStream(this, LS_VERBOSE);
      registered_ = true;
    }
  }
  ~DropDiagnostics() override { Finish(); }
  void OnLogMessage(const std::string& message) override {
    const bool rate_drop =
        message.find("Drop Frame: target bitrate") != std::string::npos;
    const bool blocked = message.find(
                             "Incoming frame dropped due to that the "
                             "encoder is blocked") != std::string::npos;
    const bool bad_timestamp =
        message.find("Same/old NTP timestamp") != std::string::npos;
    const bool retain =
        rate_drop || blocked || bad_timestamp ||
        message.find("Number of frames: captured") != std::string::npos ||
        message.find("OnBitrateUpdated, bitrate") != std::string::npos ||
        message.find("AMF_RECONFIG") != std::string::npos ||
        message.find("DroppedFrames.") != std::string::npos;
    if (!retain)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    rate_drops_ += rate_drop;
    blocked_drops_ += blocked;
    bad_timestamp_drops_ += bad_timestamp;
    // Preserve bounded raw messages; overflow is an explicit diagnostic FAIL.
    if (bytes_ + message.size() > 1024 * 1024) {
      valid_ = false;
      return;
    }
    file_ << message;
    file_.flush();
    bytes_ += message.size();
    valid_ &= file_.good();
  }
  bool Finish() {
    if (registered_) {
      // Remove waits for in-flight sink calls. Only then close its storage.
      LogMessage::RemoveLogToStream(this);
      registered_ = false;
      file_.close();
      valid_ &= !file_.fail();
    }
    return valid_;
  }
  std::string Json() const {
    std::ostringstream out;
    out << "{\"enabled\":" << (enabled_ ? "true" : "false")
        << ",\"valid\":" << (valid_ ? "true" : "false")
        << ",\"media_optimization_log_events\":" << rate_drops_
        << ",\"encoder_blocked_log_events\":" << blocked_drops_
        << ",\"bad_timestamp_log_events\":" << bad_timestamp_drops_
        << ",\"retained_bytes\":" << bytes_
        << ",\"scope\":\"opt-in upstream log events; disabled counts are "
           "not measurements; raw 60-second summaries and final statistics "
           "remain separate; not formal performance evidence\"}";
    return out.str();
  }

 private:
  const bool enabled_;
  bool registered_ = false, valid_ = true;
  uint64_t bytes_ = 0, rate_drops_ = 0, blocked_drops_ = 0;
  uint64_t bad_timestamp_drops_ = 0;
  std::mutex mutex_;
  std::ofstream file_;
};

int Run(int argc, char** argv) {
  Options options;
  if (!Parse(argc, argv, &options)) {
    std::cerr << "Usage: amf_peer_connection_smoke --role sender|receiver "
                 "--codec H264|H265 --outputdir ABSOLUTE_DIRECTORY "
                 "[--backend AMF|NVENC|QSV] "
                 "[--seconds 1..3600] [--width 1280|1920 --height 720|1080] "
                 "[--fps 30] [--source synthetic|desktop] "
                 "[--drop-diagnostics 0|1 (diagnostic overhead)] "
                 "[--ffmpeg ABSOLUTE_PATH] "
                 "[--ffmpeg-dll-dir ABSOLUTE_DIRECTORY (required for H265)]\n";
    return 2;
  }
  const std::filesystem::path directory(options.output_directory);
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  const auto result_path = directory / (options.role + "-result.json");
  const auto local_sdp =
      directory / (options.role == "sender" ? "offer.sdp" : "answer.sdp");
  if (filesystem_error || FileExists(result_path) || FileExists(local_sdp)) {
    std::cerr
        << "FAIL output directory invalid or role evidence already exists\n";
    return 2;
  }
  auto state = std::make_shared<Shared>(options);
  DropDiagnostics drop_diagnostics(
      options.drop_diagnostics,
      directory / (options.role + "-upstream-drop-diagnostics.txt"));
  PrimaryDesktopInput desktop(state);
  LogMessage::LogToDebug(LS_WARNING);
  const bool ssl_initialized = InitializeSSL();
  if (!ssl_initialized)
    state->Fail("InitializeSSL failed");
  auto network = Thread::CreateWithSocketServer();
  auto worker = Thread::Create();
  auto signaling = Thread::Create();
  bool threads_started = false;
  if (ssl_initialized && network && worker && signaling) {
    threads_started = network->Start() && worker->Start() && signaling->Start();
    if (!threads_started)
      state->Fail("WebRTC thread startup failed");
  }
  if (!threads_started && !state->failed)
    state->Fail("WebRTC thread creation failed");
  FrameSink sink(state);
  Observer observer(state, &sink);
  Endpoint endpoint;
  Stats final_stats;
  double media_wall_seconds = 0;
  bool duration_completed = false, capture_stopped = false;
  bool close_completed = false, stats_observed = false;
  int final_ice_state = 0, final_connection_state = 0;
  uint64_t stats_reports = 0;
  std::ofstream stats_jsonl(directory / (options.role + "-stats.jsonl"));
  std::ofstream counters(directory / (options.role + "-counters.csv"));
  counters
      << "elapsed_seconds,source_frames,skipped_capture_slots,encode_calls,"
         "encoded_frames,encode_errors,encoder_drops,decoder_inputs,decoded_"
         "frames,"
         "rendered_frames,nonblack_frames,outbound_bytes,inbound_bytes,"
         "stats_encoded_frames,stats_decoded_frames,packets_lost,ice_state,"
         "connection_state,dtls_connected,srtp,udp_selected\n";
  if (!stats_jsonl || !counters)
    state->Fail("stats evidence files could not be opened");
  if (threads_started && !state->failed) {
    signaling->BlockingCall([&] {
      InitializeEndpoint(&endpoint, state, network.get(), worker.get(),
                         signaling.get(), &observer);
    });
  }
  if (!state->failed && endpoint.pc) {
    if (options.role == "sender") {
      if (!WriteFile(directory / "settings.txt", SettingsText(options)))
        state->Fail("session settings write failed");
      if (!state->failed &&
          GatherAndExport(endpoint, *signaling, state, local_sdp, true))
        ApplyRemote(endpoint, *signaling, state, directory / "answer.sdp",
                    SdpType::kAnswer);
    } else {
      if (WaitFor(
              state, [&] { return FileExists(directory / "settings.txt"); }, 90,
              "sender settings timed out") &&
          ReadFile(directory / "settings.txt") != SettingsText(options))
        state->Fail(
            "sender/receiver backend, codec, dimensions, FPS, source or "
            "duration "
            "differ");
      if (!state->failed &&
          ApplyRemote(endpoint, *signaling, state, directory / "offer.sdp",
                      SdpType::kOffer)) {
        signaling->BlockingCall([&] {
          for (const auto& transceiver : endpoint.pc->GetTransceivers())
            CheckError(transceiver->SetDirectionWithError(
                           RtpTransceiverDirection::kRecvOnly),
                       state, "SetDirection recvonly");
        });
        if (!state->failed)
          GatherAndExport(endpoint, *signaling, state, local_sdp, false);
      }
    }
  }
  if (!state->failed) {
    WaitFor(
        state,
        [&] {
          return state->connection_state ==
                 static_cast<int>(
                     PeerConnectionInterface::PeerConnectionState::kConnected);
        },
        45, "real UDP ICE/DTLS connection timed out");
    if (!state->failed &&
        !WriteFile(directory / (options.role + ".connected"), "connected\n"))
      state->Fail("connection marker write failed");
  }
  std::vector<scoped_refptr<I420Buffer>> input_frames;
  if (!state->failed && options.role == "sender") {
    // Input preparation is outside the active sending interval.
    if (options.source == "synthetic")
      input_frames = MakeFrames(options);
    else
      desktop.Initialize();
    if (!state->failed &&
        WaitFor(
            state, [&] { return FileExists(directory / "receiver.connected"); },
            30, "receiver connection marker timed out") &&
        !WriteFile(directory / "media-start.signal", "start\n"))
      state->Fail("media start marker write failed");
  } else if (!state->failed) {
    WaitFor(
        state, [&] { return FileExists(directory / "media-start.signal"); }, 30,
        "sender media start marker timed out");
  }
  const auto media_start = SteadyClock::now();
  const auto media_finish = media_start + std::chrono::seconds(options.seconds);
  auto next_stats = media_start;
  uint64_t slot = 0;
  const uint64_t slots = static_cast<uint64_t>(options.seconds) * options.fps;
  while (!state->failed && SteadyClock::now() < media_finish) {
    auto now = SteadyClock::now();
    if (options.role == "sender") {
      auto due = static_cast<uint64_t>(
          std::chrono::duration<double>(now - media_start).count() *
          options.fps);
      if (due > slot) {
        state->skipped_capture_slots += std::min(due, slots) - slot;
        slot = std::min(due, slots);
      }
      if (slot >= slots)
        break;
      const auto deadline =
          media_start +
          std::chrono::nanoseconds(slot * 1000000000ULL / options.fps);
      if (now < deadline)
        std::this_thread::sleep_until(deadline);
      now = SteadyClock::now();
      if (now >= media_finish)
        break;
      due = static_cast<uint64_t>(
          std::chrono::duration<double>(now - media_start).count() *
          options.fps);
      if (due > slot) {
        state->skipped_capture_slots += due - slot;
        slot = due;
      }
      const int64_t timestamp_us = TimeMicros();
      scoped_refptr<I420Buffer> pixels =
          options.source == "desktop"
              ? desktop.Capture()
              : input_frames[slot % input_frames.size()];
      if (!pixels || state->failed)
        break;
      auto frame = VideoFrame::Builder()
                       .set_video_frame_buffer(pixels)
                       .set_timestamp_us(timestamp_us)
                       .set_rtp_timestamp(
                           static_cast<uint32_t>(timestamp_us * 90 / 1000))
                       .build();
      worker->BlockingCall([&] { endpoint.source->Push(frame); });
      ++state->source_frames;
      ++slot;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    now = SteadyClock::now();
    if (now >= next_stats && !state->failed) {
      Stats snapshot;
      if (!CollectStats(endpoint, *signaling, state, &snapshot))
        break;
      const double elapsed =
          std::chrono::duration<double>(now - media_start).count();
      stats_jsonl << "{\"elapsed_seconds\":" << elapsed
                  << ",\"report\":" << snapshot.json << "}\n";
      stats_jsonl.flush();
      counters << elapsed << ',' << state->source_frames << ','
               << state->skipped_capture_slots << ',' << state->encode_calls
               << ',' << state->encode_outputs << ',' << state->encode_errors
               << ',' << state->encoder_drops << ',' << state->decoder_inputs
               << ',' << state->decoder_outputs << ',' << state->rendered_frames
               << ',' << state->nonblack_frames << ',' << snapshot.bytes_sent
               << ',' << snapshot.bytes_received << ','
               << snapshot.frames_encoded << ',' << snapshot.frames_decoded
               << ',' << snapshot.packets_lost << ',' << state->ice_state << ','
               << state->connection_state << ',' << snapshot.dtls_connected
               << ',' << snapshot.srtp << ',' << snapshot.udp_selected << '\n';
      counters.flush();
      if (!stats_jsonl || !counters)
        state->Fail("stats/counters evidence write failed");
      final_stats = std::move(snapshot);
      stats_observed = true;
      ++stats_reports;
      next_stats += std::chrono::seconds(1);
    }
  }
  if (!state->failed && SteadyClock::now() < media_finish)
    std::this_thread::sleep_until(media_finish);
  media_wall_seconds =
      std::chrono::duration<double>(SteadyClock::now() - media_start).count();
  duration_completed = !state->failed && media_wall_seconds >= options.seconds;
  if (options.role == "sender" && threads_started) {
    // This harness has no self-rescheduling capture task. Finishing the main
    // loop prevents all future source posts; the worker barrier drains Push.
    worker->BlockingCall([] {});
    signaling->BlockingCall([&] {
      if (endpoint.source)
        endpoint.source->End();
    });
    capture_stopped = true;
  }
  // Stop the source first, then destroy its GDI capturer on the main owner
  // thread, before any PeerConnection/factory teardown.
  desktop.Stop();
  if (!state->failed && endpoint.pc) {
    if (options.role == "receiver") {
      // Allow final RTP/decoder work to finish while both peers remain open.
      std::this_thread::sleep_for(std::chrono::seconds(2));
    } else {
      WaitFor(
          state, [&] { return FileExists(directory / "receiver.media-done"); },
          15, "receiver drain timed out");
    }
    if (!state->failed &&
        CollectStats(endpoint, *signaling, state, &final_stats)) {
      stats_observed = true;
      ++stats_reports;
      stats_jsonl << "{\"final_before_close\":true,\"report\":"
                  << final_stats.json << "}\n";
    }
    state->closing = true;
    if (options.role == "sender") {
      if (!WriteFile(directory / "sender.media-done", "final stats captured\n"))
        state->Fail("sender drain marker write failed");
    } else if (!state->failed) {
      // Publish the receiver marker only after its final connected transport
      // stats have been captured. The sender may close after seeing it.
      if (!WriteFile(directory / "receiver.media-done",
                     "final stats captured\n"))
        state->Fail("receiver drain marker write failed");
      WaitFor(
          state, [&] { return FileExists(directory / "sender.media-done"); },
          15, "sender final stats timed out");
    }
  }
  final_ice_state = state->ice_state;
  final_connection_state = state->connection_state;
  state->closing = true;
  if (threads_started) {
    signaling->BlockingCall([&] {
      if (endpoint.source && !capture_stopped)
        endpoint.source->End();
      if (endpoint.sender && endpoint.pc)
        CheckError(endpoint.pc->RemoveTrackOrError(endpoint.sender), state,
                   "RemoveTrack");
      observer.Detach();
      if (endpoint.pc) {
        endpoint.pc->Close();
        close_completed = true;
      }
      endpoint.sender = nullptr;
      endpoint.local_track = nullptr;
      endpoint.pc = nullptr;
      endpoint.factory = nullptr;
      endpoint.source = nullptr;
    });
  }
  input_frames.clear();
  // No capture or stats producer remains. Factory/PC destruction above stops
  // media task queues before the shared observer/sink state can be destroyed.
  if (worker)
    worker->Stop();
  if (signaling)
    signaling->Stop();
  if (network)
    network->Stop();
  if (!drop_diagnostics.Finish())
    state->Fail("upstream drop diagnostic file failed or exceeded its bound");
  if (ssl_initialized && !CleanupSSL())
    state->Fail("CleanupSSL failed");
  stats_jsonl.close();
  counters.close();
  if (!stats_jsonl || !counters)
    state->Fail("stats evidence final flush failed");
  const uint64_t minimum_frames =
      static_cast<uint64_t>(std::min(30, options.seconds * options.fps / 2));
  if (!state->failed &&
      (!duration_completed || !close_completed || !stats_observed ||
       !final_stats.dtls_connected || !final_stats.srtp ||
       !final_stats.udp_selected || !final_stats.codec_verified))
    state->Fail(
        "runtime duration, teardown, UDP, DTLS, SRTP or negotiated codec gate "
        "failed");
  if (!state->failed && options.role == "sender" &&
      (!capture_stopped || state->encode_outputs < minimum_frames ||
       final_stats.bytes_sent == 0 || final_stats.frames_encoded == 0 ||
       state->software_identity_events != 0 || state->encode_errors != 0 ||
       !state->builtin_encoder_format_verified ||
       state->encoder_pending_peak != 1 || state->encoder_pending != 0 ||
       state->encoder_release_calls == 0 ||
       state->encoder_release_errors != 0 ||
       state->encoder_pending_after_release != 0))
    state->Fail("sender did not produce sufficient verified " +
                options.backend + " hardware media");
  if (!state->failed && options.role == "receiver" &&
      (state->decoder_outputs < minimum_frames ||
       state->rendered_frames < minimum_frames ||
       state->nonblack_frames != state->rendered_frames ||
       state->invalid_frames != 0 || final_stats.bytes_received == 0 ||
       final_stats.frames_decoded == 0 || state->decode_errors != 0 ||
       state->decoder_pending != 0 || state->decoder_release_calls == 0 ||
       state->decoder_release_errors != 0 ||
       state->decoder_pending_after_release != 0))
    state->Fail(
        "receiver native decode, dimensions or nonblack pixel gate failed");
  WriteKeySamples(directory, state);
  std::ostringstream result;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    std::ostringstream builtin_formats;
    builtin_formats << '[';
    for (size_t i = 0; i < state->builtin_encoder_formats.size(); ++i) {
      if (i)
        builtin_formats << ',';
      builtin_formats << Quote(state->builtin_encoder_formats[i]);
    }
    builtin_formats << ']';
    result
        << std::setprecision(10) << "{\"schema_version\":2,\"overall_status\":"
        << Quote(state->failed ? "FAIL" : "PASS")
        << ",\"role\":" << Quote(options.role)
        << ",\"process_id\":" << GetCurrentProcessId()
        << ",\"codec\":" << Quote(options.codec)
        << ",\"requested_backend\":" << Quote(options.backend)
        << ",\"failure\":" << Quote(state->failure)
        << ",\"scope\":\"two independent local processes, real UDP "
           "ICE/DTLS/SRTP "
           "PeerConnection video; no fake network, no manual RTP sender\""
        << ",\"source\":" << Quote(options.source) << ",\"input_source\":"
        << Quote(options.source == "desktop"
                     ? "actual primary GDI desktop, fitted ARGB-to-I420 latest "
                       "frame"
                     : "synthetic gradient plus moving square, 60-frame ring")
        << ",\"desktop_capture\":" << desktop.Json()
        << ",\"upstream_drop_diagnostics\":" << drop_diagnostics.Json()
        << ",\"width\":" << options.width << ",\"height\":" << options.height
        << ",\"requested_fps\":" << options.fps
        << ",\"requested_seconds\":" << options.seconds
        << ",\"media_wall_seconds\":" << media_wall_seconds
        << ",\"duration_completed\":" << (duration_completed ? "true" : "false")
        << ",\"source_frames\":" << state->source_frames
        << ",\"skipped_capture_slots\":" << state->skipped_capture_slots
        << ",\"capture_stopped_before_peer_connection_close\":"
        << (capture_stopped ? "true" : "false")
        << ",\"encoder_name\":" << Quote(state->encoder_name)
        << ",\"builtin_encoder_supported_formats\":" << builtin_formats.str()
        << ",\"builtin_encoder_requested_format_verified\":"
        << (state->builtin_encoder_format_verified ? "true" : "false")
        << ",\"encoder_creations\":" << state->encoder_creations
        << ",\"encode_calls\":" << state->encode_calls
        << ",\"encoded_frames\":" << state->encode_outputs
        << ",\"encoded_bytes\":" << state->encoded_bytes
        << ",\"encoded_keys\":" << state->encoded_keys
        << ",\"encode_errors\":" << state->encode_errors
        << ",\"encoder_drops\":" << state->encoder_drops
        << ",\"encoder_pending_peak\":" << state->encoder_pending_peak
        << ",\"encoder_pending_final\":" << state->encoder_pending
        << ",\"encoder_release_calls\":" << state->encoder_release_calls
        << ",\"encoder_release_errors\":" << state->encoder_release_errors
        << ",\"encoder_pending_after_release\":"
        << state->encoder_pending_after_release
        << ",\"encode_latency\":" << state->encode_latency.Json()
        << ",\"key_encode_latency\":" << state->key_encode_latency.Json()
        << ",\"delta_encode_latency\":" << state->delta_encode_latency.Json()
        << ",\"encode_to_callback_latency\":"
        << state->encode_to_callback_latency.Json()
        << ",\"encoded_callback_latency\":" << state->callback_latency.Json()
        << ",\"latency_scope\":\"single-frame synchronous Encode wrapper "
           "including hardware backend and callback forwarding; callback "
           "arrival and callback handling measured separately; percentiles "
           "are 0.1 ms histogram upper bounds, mean/max are exact; no "
           "RTCStats interval averaging\""
        << ",\"software_identity_events\":" << state->software_identity_events
        << ",\"first_encoded_rtp_timestamp\":" << state->first_encoded_timestamp
        << ",\"last_encoded_rtp_timestamp\":" << state->last_encoded_timestamp
        << ",\"decoder_name\":" << Quote(state->decoder_name)
        << ",\"decoder_scope\":"
        << Quote(options.codec == "H265"
                     ? "test-only FFmpeg HEVC software reference, dynamically "
                       "loaded; not product native HEVC decoder coverage"
                     : "native builtin WebRTC H264 decoder")
        << ",\"ffmpeg_dll_directory\":" << Quote(options.ffmpeg_dll_directory)
        << ",\"decoder_inputs\":" << state->decoder_inputs
        << ",\"decoded_frames\":" << state->decoder_outputs
        << ",\"decode_errors\":" << state->decode_errors
        << ",\"decoder_pending_peak\":" << state->decoder_pending_peak
        << ",\"decoder_pending_final\":" << state->decoder_pending
        << ",\"decoder_release_calls\":" << state->decoder_release_calls
        << ",\"decoder_release_errors\":" << state->decoder_release_errors
        << ",\"decoder_pending_after_release\":"
        << state->decoder_pending_after_release
        << ",\"rendered_frames\":" << state->rendered_frames
        << ",\"nonblack_frames\":" << state->nonblack_frames
        << ",\"invalid_frames\":" << state->invalid_frames
        << ",\"first_decoded_rtp_timestamp\":" << state->first_decoded_timestamp
        << ",\"last_decoded_rtp_timestamp\":" << state->last_decoded_timestamp
        << ",\"outbound_bytes\":" << final_stats.bytes_sent
        << ",\"inbound_bytes\":" << final_stats.bytes_received
        << ",\"stats_encoded_frames\":" << final_stats.frames_encoded
        << ",\"stats_decoded_frames\":" << final_stats.frames_decoded
        << ",\"packets_lost\":" << final_stats.packets_lost
        << ",\"stats_reports\":" << stats_reports
        << ",\"negotiated_mime_type\":" << Quote(final_stats.mime_type)
        << ",\"ice_candidates\":" << state->candidates
        << ",\"ice_state_before_close\":" << final_ice_state
        << ",\"connection_state_before_close\":" << final_connection_state
        << ",\"dtls_connected\":"
        << (final_stats.dtls_connected ? "true" : "false")
        << ",\"selected_candidate_udp\":"
        << (final_stats.udp_selected ? "true" : "false")
        << ",\"dtls_cipher\":" << Quote(final_stats.dtls_cipher)
        << ",\"srtp_cipher\":" << Quote(final_stats.srtp_cipher)
        << ",\"disconnected_events\":" << state->disconnected_events
        << ",\"peer_connection_close_completed\":"
        << (close_completed ? "true" : "false")
        << ",\"psnr_status\":\"NOT_RUN\",\"psnr_reason\":\"this harness has "
           "no source-index map after VideoStreamEncoder RTP timestamp "
           "assignment\""
        << ",\"qualification\":\"local functional smoke only; FPS, packet "
           "loss, "
           "CPU, GPU, VRAM and long stability need separate runner "
           "criteria\"}\n";
  }
  if (!WriteFile(result_path, result.str())) {
    std::cerr << "FAIL result evidence write failed\n";
    return 1;
  }
  std::cout << (state->failed ? "FAIL" : "PASS") << " role=" << options.role
            << " backend=" << options.backend
            << " encoded=" << state->encode_outputs
            << " decoded=" << state->decoder_outputs
            << " result=" << result_path.string() << '\n';
  return state->failed ? 1 : 0;
}
}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  webrtc::WinsockInitializer winsock;
  if (winsock.error() != 0) {
    std::cerr << "FAIL WSAStartup: " << winsock.error() << '\n';
    return 1;
  }
  return webrtc::Run(argc, argv);
}
