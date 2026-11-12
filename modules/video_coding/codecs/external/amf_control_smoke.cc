/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
// Serial lifecycle/control validation. This measures the complete harness CPU
// cost, including generation/decoding/IO, not encoder-only performance. It does
// not inject a driver fault or concurrently race Release with Encode.
#include <dxgi1_4.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/sps_parser.h"
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_sps_parser.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/codecs/external/ffmpeg_h265_test_decoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/bitstream_reader.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;
using Microsoft::WRL::ComPtr;
constexpr double kMinimumPsnr = 28.0;
constexpr int kWarmupCycles = 3;
constexpr int kReleaseObservationMs = 50;

struct Options {
  std::string codec = "H264", output, ffmpeg_dll_dir;
  int cycles = 10, width = 1280, height = 720;
};

std::string Quote(const std::string& value) {
  std::string result = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 32) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += static_cast<char>(c);
    }
  }
  return result + '"';
}

bool Parse(int argc, char** argv, Options* options) {
  if (argc % 2 != 1)
    return false;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--output") {
      options->output = value;
    } else if (key == "--codec") {
      options->codec = value;
    } else if (key == "--ffmpeg-dll-dir") {
      options->ffmpeg_dll_dir = value;
    } else {
      char* end = nullptr;
      const long number = std::strtol(value.c_str(), &end, 10);
      if (!end || *end || value.empty() || number < 0 || number > 1920)
        return false;
      if (key == "--cycles")
        options->cycles = static_cast<int>(number);
      else if (key == "--width")
        options->width = static_cast<int>(number);
      else if (key == "--height")
        options->height = static_cast<int>(number);
      else
        return false;
    }
  }
  return (options->codec == "H264" || options->codec == "H265") &&
         !options->output.empty() &&
         (options->codec == "H264" ||
          (!options->ffmpeg_dll_dir.empty() &&
           std::filesystem::u8path(options->ffmpeg_dll_dir).is_absolute())) &&
         (options->cycles == 10 || options->cycles == 50) &&
         ((options->width == 1280 && options->height == 720) ||
          (options->width == 1920 && options->height == 1080));
}

uint64_t Ticks(const FILETIME& value) {
  return (static_cast<uint64_t>(value.dwHighDateTime) << 32) |
         value.dwLowDateTime;
}
bool CpuTime(uint64_t* ticks) {
  FILETIME creation, exit, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    return false;
  *ticks = Ticks(kernel) + Ticks(user);
  return true;
}

struct Resource {
  std::string phase;
  int cycle = -1;
  double wall_ms = 0;
  bool process_valid = false, gpu_valid = false;
  uint32_t handles = 0;
  uint64_t private_bytes = 0, working_set_bytes = 0;
  uint64_t gpu_local_usage_bytes = 0, gpu_local_budget_bytes = 0;
  uint32_t gpu_query_hresult = 0;
};

class Resources {
 public:
  bool Initialize() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
      return false;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      const HRESULT result = factory->EnumAdapters1(index, &adapter);
      if (result == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(result))
        return false;
      DXGI_ADAPTER_DESC1 description = {};
      if (FAILED(adapter->GetDesc1(&description)) ||
          description.VendorId != 0x1002)
        continue;
      adapter_index = index;
      dedicated_memory_bytes = description.DedicatedVideoMemory;
      return SUCCEEDED(adapter.As(&adapter_));
    }
    return false;
  }
  Resource Read(std::string phase, int cycle, double wall_ms) const {
    Resource result;
    result.phase = std::move(phase);
    result.cycle = cycle;
    result.wall_ms = wall_ms;
    DWORD handles = 0;
    PROCESS_MEMORY_COUNTERS_EX memory = {};
    memory.cb = sizeof(memory);
    result.process_valid =
        GetProcessHandleCount(GetCurrentProcess(), &handles) &&
        GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory));
    result.handles = handles;
    result.private_bytes = memory.PrivateUsage;
    result.working_set_bytes = memory.WorkingSetSize;
    DXGI_QUERY_VIDEO_MEMORY_INFO gpu = {};
    const HRESULT queried = adapter_
                                ? adapter_->QueryVideoMemoryInfo(
                                      0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &gpu)
                                : E_NOINTERFACE;
    result.gpu_query_hresult = static_cast<uint32_t>(queried);
    result.gpu_valid = SUCCEEDED(queried);
    result.gpu_local_usage_bytes = gpu.CurrentUsage;
    result.gpu_local_budget_bytes = gpu.Budget;
    return result;
  }
  uint32_t adapter_index = 0;
  uint64_t dedicated_memory_bytes = 0;

 private:
  ComPtr<IDXGIAdapter3> adapter_;
};

struct Encoded {
  EncodedImage image;
  std::optional<CodecSpecificInfo> info;
};

class Collector final : public EncodedImageCallback {
 public:
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++callbacks;
    if (!allow_callback_ || GetCurrentThreadId() != owner_thread_)
      ++unexpected_callbacks;
    values_.push_back(
        {image, info ? std::optional<CodecSpecificInfo>(*info) : std::nullopt});
    return Result(Result::OK, image.RtpTimestamp());
  }
  void OnDroppedFrame(DropReason) override { ++dropped_callbacks; }
  void Begin(bool output_expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    allow_callback_ = output_expected;
  }
  std::vector<Encoded> Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    allow_callback_ = false;
    return values_;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    allow_callback_ = false;
    values_.clear();
  }
  std::atomic<uint64_t> callbacks{0}, unexpected_callbacks{0};
  std::atomic<uint64_t> dropped_callbacks{0};

 private:
  const DWORD owner_thread_ = GetCurrentThreadId();
  std::mutex mutex_;
  bool allow_callback_ = false;
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

class NativeDecoder {
 public:
  ~NativeDecoder() {
    if (!Release())
      std::cerr << "FAIL reference decoder destructor Release: " << error_
                << '\n';
  }
  bool Release() {
    bool success = true;
    if (decoder_ && decoder_->Release() != WEBRTC_VIDEO_CODEC_OK) {
      error_ = hevc_ ? "HEVC decoder Release: " + hevc_->last_error()
                     : "H264 decoder Release failed";
      success = false;
    }
    decoder_.reset();
    hevc_ = nullptr;
    collector_.frames.clear();
    return success;
  }
  bool Reset(const Options& options, int width, int height) {
    if (!Release())
      return false;
    error_.clear();
    if (options.codec == "H265") {
      auto decoder =
          std::make_unique<FfmpegH265TestDecoder>(options.ffmpeg_dll_dir);
      hevc_ = decoder.get();
      decoder_ = std::move(decoder);
    } else {
      decoder_ = H264Decoder::Create();
    }
    if (!decoder_) {
      error_ = options.codec + " decoder creation failed";
      return false;
    }
    VideoDecoder::Settings settings;
    settings.set_codec_type(options.codec == "H265" ? kVideoCodecH265
                                                    : kVideoCodecH264);
    settings.set_max_render_resolution({width, height});
    if (!decoder_->Configure(settings) ||
        decoder_->RegisterDecodeCompleteCallback(&collector_) !=
            WEBRTC_VIDEO_CODEC_OK) {
      error_ = hevc_ ? "HEVC decoder Configure: " + hevc_->last_error()
                     : "H264 decoder Configure failed";
      return false;
    }
    const auto info = decoder_->GetDecoderInfo();
    const bool identity =
        hevc_
            ? !info.is_hardware_accelerated &&
                  info.implementation_name.find("FFmpeg HEVC test software") ==
                      0
            : std::string(decoder_->ImplementationName()) == "FFmpeg";
    if (!identity)
      error_ = "reference decoder implementation identity mismatch";
    return identity;
  }
  bool Decode(const EncodedImage& image,
              const VideoFrame& reference,
              double* psnr) {
    collector_.frames.clear();
    if (!decoder_ || decoder_->Decode(image, -1) != WEBRTC_VIDEO_CODEC_OK ||
        collector_.frames.size() != 1) {
      error_ = hevc_ ? "HEVC reference Decode: " + hevc_->last_error()
                     : "H264 reference Decode/frame-count failure";
      return false;
    }
    const auto& decoded = collector_.frames[0];
    if (decoded.width() != reference.width() ||
        decoded.height() != reference.height() ||
        decoded.rtp_timestamp() != reference.rtp_timestamp()) {
      error_ = "decoded dimensions/RTP timestamp mismatch";
      return false;
    }
    *psnr = I420PSNR(&reference, &decoded);
    if (!std::isfinite(*psnr) || *psnr < kMinimumPsnr) {
      error_ = "decoded PSNR below frozen 28 dB gate";
      return false;
    }
    return true;
  }
  const std::string& error() const { return error_; }

 private:
  std::unique_ptr<VideoDecoder> decoder_;
  FfmpegH265TestDecoder* hevc_ = nullptr;
  std::string error_;
  DecodedCollector collector_;
};

VideoFrame MakeFrame(int width, int height, int index, uint32_t timestamp) {
  auto buffer = I420Buffer::Create(width, height);
  const int square_x = (index * 13) % (width - 80);
  const int square_y = (index * 7) % (height - 80);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const bool square = x >= square_x && x < square_x + 80 && y >= square_y &&
                          y < square_y + 80;
      buffer->MutableDataY()[y * buffer->StrideY() + x] =
          square ? 220 : 32 + ((x / 8 + y / 8) % 128);
    }
  }
  for (int y = 0; y < height / 2; ++y) {
    for (int x = 0; x < width / 2; ++x) {
      buffer->MutableDataU()[y * buffer->StrideU() + x] = 72 + (x / 8) % 32;
      buffer->MutableDataV()[y * buffer->StrideV() + x] = 160 + (y / 8) % 32;
    }
  }
  return VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_rtp_timestamp(timestamp)
      .set_timestamp_us(static_cast<int64_t>(timestamp) * 1000000 / 90000)
      .build();
}

bool ValidateH264Image(const Encoded& encoded,
                       int width,
                       int height,
                       uint32_t timestamp,
                       bool require_idr,
                       bool* idr) {
  const auto& image = encoded.image;
  if (!encoded.info || encoded.info->codecType != kVideoCodecH264 ||
      encoded.info->codecSpecific.H264.packetization_mode !=
          H264PacketizationMode::NonInterleaved ||
      image.RtpTimestamp() != timestamp ||
      image._encodedWidth != static_cast<uint32_t>(width) ||
      image._encodedHeight != static_cast<uint32_t>(height) || !image.size())
    return false;
  bool sps = false, pps = false, picture = false;
  *idr = false;
  const ArrayView<const uint8_t> bytes(image.data(), image.size());
  const auto indices = H264::FindNaluIndices(bytes);
  if (indices.empty() || indices.front().start_offset != 0)
    return false;
  for (const auto& index : indices) {
    if (!index.payload_size || (bytes[index.payload_start_offset] & 0x80))
      return false;
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    const auto type = H264::ParseNaluType(nal[0]);
    *idr |= type == H264::kIdr;
    picture |= type == H264::kIdr || type == H264::kSlice;
    pps |= type == H264::kPps;
    if (type == H264::kSps) {
      const auto state = SpsParser::ParseSps(nal.subview(1));
      if (nal.size() < 4 || nal[1] != 66 || nal[3] > 40 || !state ||
          state->width != static_cast<uint32_t>(width) ||
          state->height != static_cast<uint32_t>(height))
        return false;
      sps = true;
    }
  }
  return picture &&
         (image._frameType == VideoFrameType::kVideoFrameKey) == *idr &&
         encoded.info->codecSpecific.H264.idr_frame == *idr &&
         (!require_idr || (*idr && sps && pps));
}

bool ValidateH265Sps(ArrayView<const uint8_t> data, int width, int height) {
  const auto state = H265SpsParser::ParseSps(data);
  const auto rbsp = H265::ParseRbsp(data);
  BitstreamReader reader(rbsp);
  reader.ConsumeBits(4);
  const uint32_t max_sub_layers = reader.ReadBits(3);
  reader.ConsumeBits(1);
  const auto ptl = H265SpsParser::ParseProfileTierLevel(true, 0, reader);
  BitstreamReader tier_reader(rbsp);
  tier_reader.ConsumeBits(8);
  const uint32_t profile_space = tier_reader.ReadBits(2);
  const bool high_tier = tier_reader.Read<bool>();
  const bool valid_level =
      ptl && (ptl->general_level_idc == 30 || ptl->general_level_idc == 60 ||
              ptl->general_level_idc == 63 || ptl->general_level_idc == 90 ||
              ptl->general_level_idc == 93 || ptl->general_level_idc == 120);
  if (!reader.Ok() || !tier_reader.Ok() || max_sub_layers != 0 ||
      profile_space != 0 || high_tier || !ptl ||
      ptl->general_profile_idc != 1 || !valid_level ||
      ptl->general_level_idc > 120 || !state ||
      state->sps_max_sub_layers_minus1 != 0 ||
      state->bit_depth_luma_minus8 != 0 || state->chroma_format_idc != 1 ||
      state->width != static_cast<uint32_t>(width) ||
      state->height != static_cast<uint32_t>(height))
    return false;
  reader.ReadExponentialGolomb();  // sps_seq_parameter_set_id.
  reader.ReadExponentialGolomb();  // chroma_format_idc.
  reader.ReadExponentialGolomb();  // pic_width_in_luma_samples.
  reader.ReadExponentialGolomb();  // pic_height_in_luma_samples.
  if (reader.Read<bool>()) {       // conformance_window_flag.
    for (int i = 0; i < 4; ++i)
      reader.ReadExponentialGolomb();
  }
  const uint32_t luma_depth = reader.ReadExponentialGolomb();
  const uint32_t chroma_depth = reader.ReadExponentialGolomb();
  return reader.Ok() && luma_depth == 0 && chroma_depth == 0;
}

bool ValidateH265Image(const Encoded& encoded,
                       int width,
                       int height,
                       uint32_t timestamp,
                       bool require_idr,
                       bool* idr) {
  const auto& image = encoded.image;
  if (!encoded.info || encoded.info->codecType != kVideoCodecH265 ||
      image.RtpTimestamp() != timestamp ||
      image._encodedWidth != static_cast<uint32_t>(width) ||
      image._encodedHeight != static_cast<uint32_t>(height) || !image.size())
    return false;
  const ArrayView<const uint8_t> bytes(image.data(), image.size());
  const auto indices = H265::FindNaluIndices(bytes);
  if (indices.empty() || indices.front().start_offset != 0)
    return false;
  bool vps = false, sps = false, pps = false, picture = false;
  *idr = false;
  for (const auto& index : indices) {
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    if (nal.size() < 2 || (nal[0] & 0x81) != 0 || nal[1] != 1)
      return false;
    const auto type = H265::ParseNaluType(nal[0]);
    const bool is_idr = type == H265::kIdrWRadl || type == H265::kIdrNLp;
    const bool is_picture =
        type == H265::kTrailN || type == H265::kTrailR || is_idr;
    if ((!is_picture && (type < H265::kVps || type > H265::kSuffixSei)) ||
        (is_picture && nal.size() <= 2))
      return false;
    *idr |= is_idr;
    picture |= is_picture;
    vps |= type == H265::kVps;
    pps |= type == H265::kPps;
    if (type == H265::kSps) {
      if (!ValidateH265Sps(nal.subview(2), width, height))
        return false;
      sps = true;
    }
  }
  // HEVC CodecSpecificInfo only supplies codecType here. H264's union members
  // must not be read; real IRAP type and EncodedImage key metadata are checked.
  return picture &&
         (image._frameType == VideoFrameType::kVideoFrameKey) == *idr &&
         (!require_idr || (*idr && vps && sps && pps));
}

bool WriteI420(std::ofstream& file, const VideoFrame& frame) {
  const auto buffer = frame.video_frame_buffer()->ToI420();
  if (!buffer)
    return false;
  for (int row = 0; row < frame.height(); ++row)
    file.write(reinterpret_cast<const char*>(buffer->DataY() +
                                             row * buffer->StrideY()),
               frame.width());
  for (int row = 0; row < frame.height() / 2; ++row)
    file.write(reinterpret_cast<const char*>(buffer->DataU() +
                                             row * buffer->StrideU()),
               frame.width() / 2);
  for (int row = 0; row < frame.height() / 2; ++row)
    file.write(reinterpret_cast<const char*>(buffer->DataV() +
                                             row * buffer->StrideV()),
               frame.width() / 2);
  return static_cast<bool>(file);
}

struct Sample {
  std::string phase, event;
  int cycle = 0, frame_index = -1, width = 0, height = 0;
  uint32_t timestamp = 0, bitrate = 0, fps = 0;
  double wall_ms = 0, action_ms = 0, psnr = 0;
  int result = 0;
  size_t callbacks = 0, bytes = 0;
  bool output_expected = false, require_idr = false, idr = false, valid = false;
};
struct Artifact {
  int cycle = 0, width = 0, height = 0;
  size_t frames = 0;
  std::string bitstream, reference;
};

class Harness {
 public:
  Harness(const Options& options,
          std::ofstream& csv,
          std::ofstream& resource_csv)
      : options_(options), csv_(csv), resource_csv_(resource_csv) {}

  bool Run() {
    if (!resources_.Initialize())
      return Fail("AMD IDXGIAdapter3 unavailable for resource measurement");
    Snapshot("process-baseline", -1);
    auto factory = CreateBuiltinVideoEncoderFactory();
    const bool hevc = options_.codec == "H265";
    const auto environment = CreateEnvironment(std::make_unique<FieldTrials>(
        hevc ? "WebRTC-H265-HardwareEncoder/AMF/"
             : "WebRTC-H264-HardwareEncoder/AMF/"));
    const auto format = hevc ? SdpVideoFormat("H265", {{"profile-id", "1"},
                                                       {"tier-flag", "0"},
                                                       {"level-id", "120"},
                                                       {"tx-mode", "SRST"}})
                             : CreateH264Format(H264Profile::kProfileBaseline,
                                                H264Level::kLevel4, "1");
    encoder_ = factory->Create(environment, format);
    if (!encoder_)
      return Fail("builtin AMF " + options_.codec + " encoder creation failed");
    for (int cycle = 0; cycle < kWarmupCycles; ++cycle) {
      if (!Cycle("warmup", cycle))
        return CleanupFailure();
    }
    Snapshot("measurement-baseline", -1);
    measurement_started_ = true;
    measured_start_ = Clock::now();
    cpu_start_valid_ = CpuTime(&cpu_start_);
    callbacks_start_ = collector_.callbacks.load();
    for (int cycle = 0; cycle < options_.cycles; ++cycle) {
      if (!Cycle("measurement", cycle))
        return CleanupFailure();
      ++completed_cycles;
      std::cout << "CONTROL cycle=" << completed_cycles << '/'
                << options_.cycles
                << " callbacks=" << collector_.callbacks.load() << '\n';
    }
    measured_end_ = Clock::now();
    cpu_end_valid_ = CpuTime(&cpu_end_);
    measured_callbacks = collector_.callbacks.load() - callbacks_start_;
    encoder_.reset();
    collector_.Clear();
    if (!decoder_.Release())
      return Fail(decoder_.error());
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kReleaseObservationMs));
    Snapshot("post-encoder-destroy", options_.cycles);
    return collector_.unexpected_callbacks.load() == 0 &&
                   collector_.dropped_callbacks.load() == 0
               ? true
               : Fail("unexpected asynchronous/dropped callback observed");
  }

  double measured_wall_ms() const {
    return measurement_started_ && measured_end_ > measured_start_
               ? Milliseconds(measured_end_ - measured_start_).count()
               : 0;
  }
  double process_cpu_ms() const {
    return cpu_start_valid_ && cpu_end_valid_ && cpu_end_ >= cpu_start_
               ? (cpu_end_ - cpu_start_) / 10000.0
               : 0;
  }
  bool cpu_valid() const { return cpu_start_valid_ && cpu_end_valid_; }
  const Resources& resources() const { return resources_; }
  uint64_t callbacks() const { return collector_.callbacks.load(); }
  uint64_t unexpected_callbacks() const {
    return collector_.unexpected_callbacks.load();
  }
  uint64_t dropped_callbacks() const {
    return collector_.dropped_callbacks.load();
  }

  std::string error;
  int completed_cycles = 0;
  uint64_t measured_callbacks = 0;
  size_t decoded_frames = 0, invalid_calls = 0, paused_calls = 0;
  double minimum_psnr = 100;
  std::vector<Sample> samples;
  std::vector<Resource> resource_samples;
  std::vector<Artifact> artifacts;

 private:
  bool Fail(std::string message) {
    if (error.empty())
      error = std::move(message);
    return false;
  }
  bool CleanupFailure() {
    if (encoder_ && encoder_->Release() != WEBRTC_VIDEO_CODEC_OK)
      error += " | cleanup encoder Release failed";
    encoder_.reset();
    collector_.Clear();
    if (!decoder_.Release())
      error += " | " + decoder_.error();
    measured_end_ = Clock::now();
    cpu_end_valid_ = CpuTime(&cpu_end_);
    measured_callbacks = measurement_started_
                             ? collector_.callbacks.load() - callbacks_start_
                             : 0;
    Snapshot("failure-cleanup", cycle_);
    return false;
  }
  bool Identity() {
    const auto info = encoder_->GetEncoderInfo();
    return info.is_hardware_accelerated &&
                   info.implementation_name.find("AMF " + options_.codec) !=
                       std::string::npos
               ? true
               : Fail("encoder identity switched away from AMF hardware " +
                      options_.codec);
  }
  void Record(Sample sample) {
    sample.phase = phase_;
    sample.cycle = cycle_;
    sample.width = width_;
    sample.height = height_;
    sample.bitrate = bitrate_;
    sample.fps = fps_;
    sample.wall_ms = Milliseconds(Clock::now() - process_start_).count();
    csv_ << sample.phase << ',' << sample.cycle << ',' << sample.event << ','
         << sample.frame_index << ',' << sample.timestamp << ',' << sample.width
         << ',' << sample.height << ',' << sample.bitrate << ',' << sample.fps
         << ',' << sample.wall_ms << ',' << sample.action_ms << ','
         << sample.result << ',' << sample.callbacks << ',' << sample.bytes
         << ',' << sample.output_expected << ',' << sample.require_idr << ','
         << sample.idr << ',' << sample.psnr << ',' << sample.valid << '\n';
    csv_.flush();
    samples.push_back(std::move(sample));
    if (!csv_)
      Fail("raw control CSV write failed");
  }
  void Snapshot(const std::string& phase, int cycle) {
    const auto sample = resources_.Read(
        phase, cycle, Milliseconds(Clock::now() - process_start_).count());
    resource_csv_ << sample.phase << ',' << sample.cycle << ','
                  << sample.wall_ms << ',' << sample.process_valid << ','
                  << sample.handles << ',' << sample.private_bytes << ','
                  << sample.working_set_bytes << ',' << sample.gpu_valid << ','
                  << sample.gpu_local_usage_bytes << ','
                  << sample.gpu_local_budget_bytes << ','
                  << sample.gpu_query_hresult << '\n';
    resource_csv_.flush();
    resource_samples.push_back(sample);
    if (!sample.process_valid || !sample.gpu_valid || !resource_csv_)
      Fail("resource sampling/CSV failed; resource status is UNKNOWN");
  }
  bool Action(const std::string& event,
              int actual,
              int expected,
              Clock::time_point begin,
              uint64_t callbacks_before) {
    Sample sample;
    sample.event = event;
    sample.result = actual;
    sample.action_ms = Milliseconds(Clock::now() - begin).count();
    sample.callbacks = collector_.callbacks.load() - callbacks_before;
    sample.valid = actual == expected && sample.callbacks == 0;
    Record(sample);
    return sample.valid ? error.empty()
                        : Fail(event + " result/callback mismatch");
  }
  bool Rate(uint32_t bps, uint32_t fps) {
    const auto before = collector_.callbacks.load();
    const auto begin = Clock::now();
    VideoBitrateAllocation allocation;
    allocation.SetBitrate(0, 0, bps);
    encoder_->SetRates(VideoEncoder::RateControlParameters(allocation, fps));
    bitrate_ = bps;
    fps_ = fps;
    return Action("set-rates", 0, 0, begin, before);
  }
  bool Frame(const std::string& event,
             bool force_idr,
             bool require_idr,
             bool expect_output,
             int expected_status = WEBRTC_VIDEO_CODEC_OK,
             std::optional<bool> expected_idr = std::nullopt) {
    if (expect_output && !Identity())
      return false;
    timestamp_ += 90000 / fps_;
    const auto input = MakeFrame(width_, height_, frame_index_, timestamp_);
    Sample sample;
    sample.event = event;
    sample.frame_index = frame_index_++;
    sample.timestamp = timestamp_;
    sample.output_expected = expect_output;
    sample.require_idr = require_idr;
    collector_.Begin(expect_output);
    const std::vector<VideoFrameType> key = {VideoFrameType::kVideoFrameKey};
    const auto begin = Clock::now();
    sample.result = encoder_->Encode(input, force_idr ? &key : nullptr);
    sample.action_ms = Milliseconds(Clock::now() - begin).count();
    const auto encoded = collector_.Finish();
    sample.callbacks = encoded.size();
    bool valid = sample.result == expected_status &&
                 encoded.size() == (expect_output ? 1u : 0u) &&
                 collector_.unexpected_callbacks.load() == 0;
    if (valid && expect_output) {
      sample.bytes = encoded[0].image.size();
      valid = Identity() &&
              (options_.codec == "H265"
                   ? ValidateH265Image(encoded[0], width_, height_, timestamp_,
                                       require_idr, &sample.idr)
                   : ValidateH264Image(encoded[0], width_, height_, timestamp_,
                                       require_idr, &sample.idr));
      if (valid && expected_idr)
        valid = sample.idr == *expected_idr;
      if (valid && sample.idr)
        valid = decoder_.Reset(options_, width_, height_);
      if (valid)
        valid = decoder_.Decode(encoded[0].image, input, &sample.psnr);
      if (!valid && !decoder_.error().empty())
        Fail(decoder_.error());
      if (valid) {
        ++decoded_frames;
        minimum_psnr = std::min(minimum_psnr, sample.psnr);
        if (bitstream_.is_open()) {
          bitstream_.write(
              reinterpret_cast<const char*>(encoded[0].image.data()),
              encoded[0].image.size());
          valid = static_cast<bool>(bitstream_) && WriteI420(reference_, input);
          if (valid)
            ++artifacts.back().frames;
        }
      }
    }
    sample.valid = valid;
    Record(sample);
    return valid ? error.empty()
                 : Fail(event + " encode/identity/metadata/decode failure");
  }
  bool Cycle(const std::string& phase, int cycle) {
    phase_ = phase;
    cycle_ = cycle;
    const bool original = cycle % 2 == 0;
    width_ = original ? options_.width : (options_.width == 1280 ? 1920 : 1280);
    height_ = width_ == 1280 ? 720 : 1080;
    bitrate_ = 3000000;
    fps_ = 30;
    if (phase == "measurement" && cycle < 2) {
      const std::string stem =
          options_.output + "-cycle-" + std::to_string(cycle) + "-" +
          std::to_string(width_) + "x" + std::to_string(height_);
      artifacts.push_back(
          {cycle, width_, height_, 0,
           stem + (options_.codec == "H265" ? ".h265" : ".h264"),
           stem + ".reference.i420"});
      bitstream_.open(artifacts.back().bitstream, std::ios::binary);
      reference_.open(artifacts.back().reference, std::ios::binary);
      if (!bitstream_ || !reference_)
        return Fail("short independent decoder artifacts could not be opened");
    }
    VideoCodec codec;
    codec.codecType =
        options_.codec == "H265" ? kVideoCodecH265 : kVideoCodecH264;
    codec.width = width_;
    codec.height = height_;
    codec.startBitrate = bitrate_ / 1000;
    codec.minBitrate = 30;
    codec.maxBitrate = 5000;
    codec.maxFramerate = fps_;
    codec.active = true;
    codec.numberOfSimulcastStreams = 0;
    codec.SetFrameDropEnabled(false);
    if (options_.codec == "H264") {
      *codec.H264() = VideoEncoder::GetDefaultH264Settings();
      codec.H264()->numberOfTemporalLayers = 1;
    }
    const auto before = collector_.callbacks.load();
    auto begin = Clock::now();
    int result = encoder_->RegisterEncodeCompleteCallback(&collector_);
    if (!Action("register", result, WEBRTC_VIDEO_CODEC_OK, begin, before))
      return false;
    begin = Clock::now();
    const auto init_callbacks_before = collector_.callbacks.load();
    result = encoder_->InitEncode(
        &codec,
        VideoEncoder::Settings(VideoEncoder::Capabilities(false), 1, 1200));
    if (!Action("init", result, WEBRTC_VIDEO_CODEC_OK, begin,
                init_callbacks_before) ||
        !Identity() || !Frame("initial", false, true, true) ||
        !Frame("delta", false, false, true) ||
        !Frame("forced-idr", true, true, true) ||
        !Frame("delta", false, false, true))
      return false;
    for (const uint32_t bps : {2000000u, 1000000u, 3000000u}) {
      if (!Rate(bps, 30) ||
          !Frame("rate-update", false, false, true, WEBRTC_VIDEO_CODEC_OK,
                 false) ||
          !Frame("delta", false, false, true))
        return false;
    }
    // Real BWE changes by hundreds of bits/s. They must update the live CBR
    // session without restarting it or injecting another IDR burst.
    for (const uint32_t bps : {2999100u, 2999900u, 2999000u, 3000000u}) {
      if (!Rate(bps, 30) ||
          !Frame("micro-rate-update", false, false, true, WEBRTC_VIDEO_CODEC_OK,
                 false) ||
          !Frame("delta", false, false, true))
        return false;
    }
    for (const uint32_t fps : {15u, 20u, 30u}) {
      if (!Rate(3000000, fps) || !Frame("fps-reopen", false, true, true) ||
          !Frame("delta", false, false, true))
        return false;
    }
    if (!Rate(0, 30))
      return false;
    for (int paused = 0; paused < 3; ++paused) {
      ++paused_calls;
      if (!Frame("paused", false, false, false))
        return false;
    }
    if (!Rate(3000000, 30) || !Frame("resumed", false, true, true) ||
        !Frame("delta", false, false, true))
      return false;
    collector_.Clear();
    if (!decoder_.Release())
      return Fail(decoder_.error());
    for (int release = 0; release < 2; ++release) {
      const auto count = collector_.callbacks.load();
      begin = Clock::now();
      result = encoder_->Release();
      if (!Action("release", result, WEBRTC_VIDEO_CODEC_OK, begin, count))
        return false;
    }
    ++invalid_calls;
    if (!Frame("after-release", false, false, false,
               WEBRTC_VIDEO_CODEC_UNINITIALIZED))
      return false;
    collector_.Clear();
    const auto count = collector_.callbacks.load();
    begin = Clock::now();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kReleaseObservationMs));
    if (!Action("release-observation", 0, 0, begin, count))
      return false;
    if (bitstream_.is_open()) {
      bitstream_.close();
      reference_.close();
      if (!bitstream_ || !reference_)
        return Fail("short artifact close/write failure");
    }
    Snapshot(phase + "-release", cycle);
    return error.empty();
  }

  const Options& options_;
  std::ofstream& csv_;
  std::ofstream& resource_csv_;
  const Clock::time_point process_start_ = Clock::now();
  Clock::time_point measured_start_{}, measured_end_{};
  uint64_t cpu_start_ = 0, cpu_end_ = 0, callbacks_start_ = 0;
  bool cpu_start_valid_ = false, cpu_end_valid_ = false;
  bool measurement_started_ = false;
  Resources resources_;
  Collector collector_;
  NativeDecoder decoder_;
  std::unique_ptr<VideoEncoder> encoder_;
  std::ofstream bitstream_, reference_;
  std::string phase_;
  int cycle_ = -1, width_ = 1280, height_ = 720, frame_index_ = 0;
  uint32_t bitrate_ = 3000000, fps_ = 30, timestamp_ = 90000;
};

double Percentile(std::vector<double> values, double fraction) {
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  const size_t rank = static_cast<size_t>(std::ceil(values.size() * fraction));
  return values[std::max<size_t>(1, rank) - 1];
}

int Run(const Options& options) {
  const std::filesystem::path prefix(options.output);
  for (const auto& suffix : {".json", ".csv", ".resources.csv"}) {
    if (std::filesystem::exists(options.output + suffix)) {
      std::cerr << "Refusing to overwrite evidence\n";
      return 1;
    }
  }
  for (int cycle = 0; cycle < 2; ++cycle) {
    const int width =
        cycle == 0 ? options.width : (options.width == 1280 ? 1920 : 1280);
    const int height = width == 1280 ? 720 : 1080;
    const auto stem = options.output + "-cycle-" + std::to_string(cycle) + "-" +
                      std::to_string(width) + "x" + std::to_string(height);
    if (std::filesystem::exists(
            stem + (options.codec == "H265" ? ".h265" : ".h264")) ||
        std::filesystem::exists(stem + ".reference.i420")) {
      std::cerr << "Refusing to overwrite short artifacts\n";
      return 1;
    }
  }
  std::error_code directory_error;
  if (!prefix.parent_path().empty())
    std::filesystem::create_directories(prefix.parent_path(), directory_error);
  if (directory_error)
    return 1;
  std::ofstream csv(options.output + ".csv"),
      resource_csv(options.output + ".resources.csv");
  if (!csv || !resource_csv)
    return 1;
  csv << std::fixed << std::setprecision(9)
      << "phase,cycle,event,input_frame_index,rtp_timestamp,width,height,"
         "requested_bitrate_bps,requested_fps,wall_ms,action_ms,result,"
         "callbacks,"
         "bytes,output_expected,require_idr,idr,psnr_db,valid\n";
  resource_csv << std::fixed << std::setprecision(9)
               << "phase,cycle,wall_ms,process_valid,handles,private_bytes,"
                  "working_set_bytes,gpu_valid,gpu_local_usage_bytes,"
                  "gpu_local_budget_bytes,gpu_query_hresult\n";
  Harness harness(options, csv, resource_csv);
  const bool control_ok = harness.Run();
  std::vector<double> encode_ms;
  for (const auto& sample : harness.samples) {
    if (sample.phase == "measurement" && sample.output_expected && sample.valid)
      encode_ms.push_back(sample.action_ms);
  }
  SYSTEM_INFO system = {};
  GetSystemInfo(&system);
  const double wall_ms = harness.measured_wall_ms();
  const double cpu_ms = harness.process_cpu_ms();
  const bool resource_valid =
      std::all_of(harness.resource_samples.begin(),
                  harness.resource_samples.end(), [](const Resource& sample) {
                    return sample.process_valid && sample.gpu_valid;
                  });
  const bool hevc = options.codec == "H265";
  std::ofstream output(options.output + ".json");
  output
      << std::fixed << std::setprecision(9)
      << "{\n\"schema_version\":2,\"status\":"
      << Quote(control_ok && harness.error.empty() ? "PASS" : "FAIL")
      << ",\"scope\":"
      << Quote("serial local AMF " + options.codec +
               " control/lifecycle and AnnexB reference decode")
      << ",\"error\":" << Quote(harness.error)
      << ",\"codec\":" << Quote(options.codec)
      << ",\"implementation_required\":"
      << Quote("AMF " + options.codec + " hardware") << ",\"field_trial\":"
      << Quote(hevc ? "WebRTC-H265-HardwareEncoder/AMF/"
                    : "WebRTC-H264-HardwareEncoder/AMF/")
      << ",\"profile\":" << Quote(hevc ? "Main" : "Baseline")
      << (hevc ? ",\"general_level_idc\":120,\"tier_flag\":0,\"tx_mode\":"
                 "\"SRST\""
               : ",\"level_idc\":40")
      << ",\"reference_decoder\":"
      << Quote(hevc ? "FFmpeg HEVC test software via explicit DLLs"
                    : "WebRTC native FFmpeg H264")
      << ",\"ffmpeg_dll_dir\":" << Quote(options.ffmpeg_dll_dir)
      << ",\"warmup_cycles\":" << kWarmupCycles
      << ",\"requested_cycles\":" << options.cycles
      << ",\"completed_cycles\":" << harness.completed_cycles
      << ",\"callbacks_total_including_warmup\":" << harness.callbacks()
      << ",\"measurement_callbacks\":" << harness.measured_callbacks
      << ",\"unexpected_callbacks\":" << harness.unexpected_callbacks()
      << ",\"dropped_callbacks\":" << harness.dropped_callbacks()
      << ",\"decoded_frames_including_warmup\":" << harness.decoded_frames
      << ",\"paused_encode_calls_including_warmup\":" << harness.paused_calls
      << ",\"invalid_after_release_calls_including_warmup\":"
      << harness.invalid_calls
      << ",\"post_release_observation_ms_per_cycle\":" << kReleaseObservationMs
      << ",\"minimum_psnr_db\":"
      << (harness.decoded_frames ? harness.minimum_psnr : 0)
      << ",\"minimum_psnr_required_db\":" << kMinimumPsnr
      << ",\"measurement_wall_ms\":" << wall_ms
      << ",\"process_cpu_valid\":" << (harness.cpu_valid() ? "true" : "false")
      << ",\"process_cpu_ms\":" << cpu_ms
      << ",\"logical_processors\":" << system.dwNumberOfProcessors
      << ",\"process_cpu_percent\":"
      << (wall_ms > 0 && system.dwNumberOfProcessors
              ? 100 * cpu_ms / wall_ms / system.dwNumberOfProcessors
              : 0)
      << ",\"control_workflow_output_fps\":"
      << (wall_ms > 0 ? 1000 * harness.measured_callbacks / wall_ms : 0)
      << ",\"cpu_scope\":\"full serial control workflow including generation, "
         "reference decode, init/rates/release, file IO and observation waits; "
         "excludes 3 warmup cycles\""
      << ",\"encode_latency_scope\":\"synchronous Encode call including "
         "adapter/backend/owned callback; excludes generation/decode/file IO\""
      << ",\"encode_call_ms\":{\"samples\":" << encode_ms.size()
      << ",\"p50\":" << Percentile(encode_ms, .50)
      << ",\"p95\":" << Percentile(encode_ms, .95)
      << ",\"p99\":" << Percentile(encode_ms, .99)
      << ",\"max\":" << Percentile(encode_ms, 1) << '}'
      << ",\"resource_measurement_status\":"
      << Quote(resource_valid && !harness.resource_samples.empty() ? "MEASURED"
                                                                   : "UNKNOWN")
      << ",\"resource_acceptance_status\":\"REVIEW_REQUIRED\""
      << ",\"resource_thresholds_applied\":false"
      << ",\"resource_scope\":\"after serial Release x2, callback reference "
         "clearing and decoder Release; AMD adapter node0 process-local DXGI "
         "LOCAL usage\""
      << ",\"resource_samples\":" << harness.resource_samples.size()
      << ",\"gpu_adapter_index\":" << harness.resources().adapter_index
      << ",\"gpu_dedicated_memory_bytes\":"
      << harness.resources().dedicated_memory_bytes
      << ",\"raw_csv\":" << Quote(options.output + ".csv")
      << ",\"resource_csv\":" << Quote(options.output + ".resources.csv")
      << ",\"rtp_network_gate\":\"NOT_RUN\",\"driver_fault_gate\":\"NOT_RUN\""
      << ",\"concurrent_teardown_gate\":\"NOT_RUN\",\"long_stability_gate\":"
         "\"NOT_RUN\""
      << ",\"independent_decoder_artifacts\":[";
  for (size_t index = 0; index < harness.artifacts.size(); ++index) {
    const auto& artifact = harness.artifacts[index];
    if (index)
      output << ',';
    output << "{\"cycle\":" << artifact.cycle << ",\"width\":" << artifact.width
           << ",\"height\":" << artifact.height
           << ",\"frames\":" << artifact.frames
           << ",\"bitstream\":" << Quote(artifact.bitstream)
           << ",\"reference_i420\":" << Quote(artifact.reference)
           << ",\"reference_frame_order\":\"successful output order; no "
              "paused/invalid input frames\"}";
  }
  output << "]\n}\n";
  output.close();
  csv.close();
  resource_csv.close();
  if (!output || !csv || !resource_csv)
    return 1;
  std::cout << (control_ok && harness.error.empty() ? "PASS" : "FAIL")
            << " control=" << options.output
            << ".json resource_acceptance=REVIEW_REQUIRED\n";
  return control_ok && harness.error.empty() ? 0 : 1;
}
}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  webrtc::Options options;
  if (!webrtc::Parse(argc, argv, &options)) {
    std::cerr << "Usage: amf_control_smoke --codec H264|H265 --output PREFIX "
                 "[--cycles 10|50] [--width 1280 --height 720 | --width 1920 "
                 "--height 1080] [--ffmpeg-dll-dir ABSOLUTE_DIR (required for "
                 "H265)]\n";
    return 1;
  }
  webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
  return webrtc::Run(options);
}
