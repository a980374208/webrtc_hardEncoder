/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
// Measurement-only product factory probe. No RTP, network or disk in the
// measured Encode path. Synthetic input generation and warmup are excluded.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "api/array_view.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h265/h265_common.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/scale.h"

namespace webrtc {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;
constexpr int kRingFrames = 60;

struct Options {
  int width = 640, height = 360, bitrate = 600000, fps = 30;
  int seconds = 20, warmup_seconds = 2;
  int key_interval = 60;
  std::string mode = "paced", workload = "screen", output;
  std::string tuning = "baseline", input, bitstream;
  int input_width = 1920, input_height = 1080;
};
struct Sample {
  uint64_t slot = 0;
  uint32_t timestamp = 0;
  double start_ms = 0, late_ms = 0, encode_ms = 0;
  size_t bytes = 0;
  bool key = false, valid = false;
};
struct Cpu {
  uint64_t process = 0, system = 0, idle = 0;
};
uint64_t Ticks(const FILETIME& time) {
  return (static_cast<uint64_t>(time.dwHighDateTime) << 32) |
         time.dwLowDateTime;
}
bool ReadCpu(Cpu* cpu) {
  FILETIME creation, exit, kernel, user, idle, system_kernel, system_user;
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) ||
      !GetSystemTimes(&idle, &system_kernel, &system_user))
    return false;
  cpu->process = Ticks(kernel) + Ticks(user);
  cpu->system = Ticks(system_kernel) + Ticks(system_user);
  cpu->idle = Ticks(idle);
  return true;
}
std::string Quote(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 32) {
      const char hex[] = "0123456789abcdef";
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += static_cast<char>(c);
    }
  }
  return result + "\"";
}
bool Parse(int argc, char** argv, Options* options) {
  if (argc % 2 != 1)
    return false;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--output")
      options->output = value;
    else if (key == "--mode")
      options->mode = value;
    else if (key == "--workload")
      options->workload = value;
    else if (key == "--tuning")
      options->tuning = value;
    else if (key == "--input")
      options->input = value;
    else if (key == "--bitstream")
      options->bitstream = value;
    else {
      char* end = nullptr;
      const long number = std::strtol(value.c_str(), &end, 10);
      if (!end || *end || value.empty() || number < 0 || number > 65535000)
        return false;
      const int integer = static_cast<int>(number);
      if (key == "--width")
        options->width = integer;
      else if (key == "--height")
        options->height = integer;
      else if (key == "--bitrate")
        options->bitrate = integer;
      else if (key == "--fps")
        options->fps = integer;
      else if (key == "--seconds")
        options->seconds = integer;
      else if (key == "--warmup-seconds")
        options->warmup_seconds = integer;
      else if (key == "--input-width")
        options->input_width = integer;
      else if (key == "--input-height")
        options->input_height = integer;
      else if (key == "--key-interval")
        options->key_interval = integer;
      else
        return false;
    }
  }
  return !options->output.empty() && options->width >= 16 &&
         options->width <= 1920 && options->height >= 16 &&
         options->height <= 1080 && options->width % 2 == 0 &&
         options->height % 2 == 0 && options->bitrate > 0 &&
         options->fps >= 1 && options->fps <= 60 && options->seconds >= 1 &&
         options->seconds <= 120 && options->warmup_seconds >= 1 &&
         options->warmup_seconds <= 10 && options->key_interval >= 1 &&
         options->key_interval <= 900 &&
         (options->mode == "paced" || options->mode == "capacity" ||
          options->mode == "sequence") &&
         (options->workload == "screen" || options->workload == "texture" ||
          options->workload == "desktop") &&
         (options->tuning == "baseline" || options->tuning == "wpp4" ||
          options->tuning == "wpp8" || options->tuning == "wpp16" ||
          options->tuning == "wpp8-nostrict" ||
          options->tuning == "baseline-cu16" ||
          options->tuning == "wpp8-cu16" || options->tuning == "wpp8-rdskip") &&
         (options->workload != "desktop" || !options->input.empty()) &&
         (options->input.empty() ||
          (options->workload == "desktop" && options->input_width >= 16 &&
           options->input_width <= 1920 && options->input_height >= 16 &&
           options->input_height <= 1080 && options->input_width % 2 == 0 &&
           options->input_height % 2 == 0)) &&
         (options->bitstream.empty() || options->mode == "sequence");
}

std::vector<scoped_refptr<I420Buffer>> LoadSequence(const Options& options) {
  std::ifstream input(options.input, std::ios::binary | std::ios::ate);
  if (!input)
    return {};
  const size_t luma =
      static_cast<size_t>(options.input_width) * options.input_height;
  const size_t frame_bytes = luma * 3 / 2;
  const auto length = input.tellg();
  if (length <= 0 || static_cast<uint64_t>(length) % frame_bytes != 0 ||
      static_cast<uint64_t>(length) / frame_bytes > 180)
    return {};
  const size_t count = static_cast<size_t>(length) / frame_bytes;
  input.seekg(0);
  std::vector<scoped_refptr<I420Buffer>> ring;
  std::vector<uint8_t> bytes(frame_bytes);
  for (size_t index = 0; index < count; ++index) {
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
      return {};
    auto frame = I420Buffer::Create(options.width, options.height);
    if (libyuv::I420Scale(
            bytes.data(), options.input_width, bytes.data() + luma,
            options.input_width / 2, bytes.data() + luma * 5 / 4,
            options.input_width / 2, options.input_width, options.input_height,
            frame->MutableDataY(), frame->StrideY(), frame->MutableDataU(),
            frame->StrideU(), frame->MutableDataV(), frame->StrideV(),
            options.width, options.height, libyuv::kFilterBox) != 0)
      return {};
    ring.push_back(std::move(frame));
  }
  return ring;
}

uint32_t Texture(uint32_t x, uint32_t y) {
  uint32_t value = x * 0x9e3779b9u + y * 0x85ebca6bu + 0x13579bdu;
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  return value;
}
std::vector<scoped_refptr<I420Buffer>> Generate(const Options& options) {
  std::vector<scoped_refptr<I420Buffer>> ring;
  ring.reserve(kRingFrames);
  for (int index = 0; index < kRingFrames; ++index) {
    auto frame = I420Buffer::Create(options.width, options.height);
    const int square_x = (index * 13) % std::max(1, options.width - 80);
    const int square_y = (index * 7) % std::max(1, options.height - 80);
    for (int y = 0; y < options.height; ++y) {
      for (int x = 0; x < options.width; ++x) {
        int pixel;
        if (options.workload == "screen") {
          const bool square = x >= square_x && x < square_x + 80 &&
                              y >= square_y && y < square_y + 80;
          pixel = square ? 220 : 32 + ((x / 8 + y / 8) % 128);
        } else {
          // A translated deterministic textured surface, not independent
          // per-frame random noise. The 60-frame ring repeats every 2s at 30Hz.
          const uint32_t tx =
              static_cast<uint32_t>((x + index * 3) % options.width);
          const uint32_t ty =
              static_cast<uint32_t>((y + index * 2) % options.height);
          pixel = 16 + static_cast<int>(Texture(tx / 2, ty / 2) % 220);
        }
        frame->MutableDataY()[y * frame->StrideY() + x] =
            static_cast<uint8_t>(pixel);
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
    ring.push_back(std::move(frame));
  }
  return ring;
}

class Collector final : public EncodedImageCallback {
 public:
  int width = 0, height = 0, count = 0;
  uint32_t timestamp = 0;
  size_t bytes = 0;
  bool valid = false, key = false;
  bool save_packet = false;
  std::vector<uint8_t> packet;
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* info) override {
    ++count;
    bytes = image.size();
    key = image.FrameType() == VideoFrameType::kVideoFrameKey;
    valid = count == 1 && image.RtpTimestamp() == timestamp &&
            image._encodedWidth == static_cast<uint32_t>(width) &&
            image._encodedHeight == static_cast<uint32_t>(height) &&
            image.data() && bytes > 0 && info &&
            info->codecType == kVideoCodecH265;
    if (valid) {
      bool idr = false, vps = false, sps = false, pps = false;
      for (const auto& nal : H265::FindNaluIndices(
               ArrayView<const uint8_t>(image.data(), image.size()))) {
        if (nal.payload_size < 2) {
          valid = false;
          break;
        }
        const auto type =
            H265::ParseNaluType(image.data()[nal.payload_start_offset]);
        idr |= type == H265::kIdrWRadl || type == H265::kIdrNLp;
        vps |= type == H265::kVps;
        sps |= type == H265::kSps;
        pps |= type == H265::kPps;
      }
      valid &= key == idr && (!key || (vps && sps && pps));
    }
    if (save_packet && valid)
      packet.assign(image.data(), image.data() + image.size());
    return Result(valid ? Result::OK : Result::ERROR_SEND_FAILED, timestamp);
  }
};

bool Identity(VideoEncoder& encoder) {
  const auto info = encoder.GetEncoderInfo();
  return !info.is_hardware_accelerated &&
         info.implementation_name == "x265 H265";
}
Sample Encode(VideoEncoder& encoder,
              Collector& collector,
              const std::vector<scoped_refptr<I420Buffer>>& ring,
              const Options& options,
              uint64_t slot,
              uint64_t base,
              bool request_key) {
  collector.count = 0;
  collector.valid = false;
  collector.bytes = 0;
  collector.key = false;
  collector.timestamp =
      90000u + static_cast<uint32_t>((base + slot + 1) * 90000 / options.fps);
  const auto frame = VideoFrame::Builder()
                         .set_video_frame_buffer(ring[slot % ring.size()])
                         .set_rtp_timestamp(collector.timestamp)
                         .set_timestamp_us(static_cast<int64_t>(base + slot) *
                                           1000000 / options.fps)
                         .build();
  const std::vector<VideoFrameType> types{
      request_key ? VideoFrameType::kVideoFrameKey
                  : VideoFrameType::kVideoFrameDelta};
  const auto start = Clock::now();
  const int32_t result = encoder.Encode(frame, &types);
  const auto end = Clock::now();
  Sample sample;
  sample.slot = slot;
  sample.timestamp = collector.timestamp;
  sample.encode_ms = Milliseconds(end - start).count();
  sample.bytes = collector.bytes;
  sample.key = collector.key;
  sample.valid = result == WEBRTC_VIDEO_CODEC_OK && collector.count == 1 &&
                 collector.valid && (!request_key || collector.key) &&
                 Identity(encoder);
  return sample;
}

double Percentile(const std::vector<double>& sorted, double fraction) {
  if (sorted.empty())
    return 0;
  return sorted[static_cast<size_t>(std::ceil(fraction * sorted.size())) - 1];
}
void Distribution(std::ostream& output,
                  const std::vector<Sample>& samples,
                  int kind) {
  std::vector<double> sorted;
  double total = 0;
  for (const auto& sample : samples) {
    if (kind == 0 || (kind == 1 && sample.key) || (kind == 2 && !sample.key)) {
      sorted.push_back(sample.encode_ms);
      total += sample.encode_ms;
    }
  }
  std::sort(sorted.begin(), sorted.end());
  output << "{\"samples\":" << sorted.size()
         << ",\"mean\":" << (sorted.empty() ? 0 : total / sorted.size())
         << ",\"p50\":" << Percentile(sorted, 0.5)
         << ",\"p95\":" << Percentile(sorted, 0.95)
         << ",\"p99\":" << Percentile(sorted, 0.99)
         << ",\"max\":" << (sorted.empty() ? 0 : sorted.back()) << "}";
}

int Run(const Options& options) {
  const auto ring =
      options.input.empty() ? Generate(options) : LoadSequence(options);
  if (ring.empty()) {
    std::cerr << "Unable to load the bounded packed I420 sequence\n";
    return 1;
  }
  if (!options.bitstream.empty() &&
      (std::filesystem::exists(options.bitstream) ||
       std::filesystem::exists(options.bitstream + ".reference.i420") ||
       std::filesystem::exists(options.bitstream + ".keys.json"))) {
    std::cerr << "Refusing to overwrite quality audit bitstream\n";
    return 1;
  }
  const int level = options.width * options.height > 1280 * 720 ? 120 : 93;
  auto environment = CreateEnvironment(
      std::make_unique<FieldTrials>("WebRTC-H265-Encoder/x265/"
                                    "WebRTC-X265-ExperimentalTuning/" +
                                    options.tuning + "/"));
  auto factory = CreateBuiltinVideoEncoderFactory();
  auto encoder = factory->Create(
      environment, SdpVideoFormat("H265", {{"profile-id", "1"},
                                           {"tier-flag", "0"},
                                           {"level-id", std::to_string(level)},
                                           {"tx-mode", "SRST"}}));
  Collector collector;
  collector.width = options.width;
  collector.height = options.height;
  VideoCodec codec;
  codec.codecType = kVideoCodecH265;
  codec.width = static_cast<uint16_t>(options.width);
  codec.height = static_cast<uint16_t>(options.height);
  codec.startBitrate = (options.bitrate + 999) / 1000;
  codec.minBitrate = 30;
  codec.maxBitrate = codec.startBitrate;
  codec.maxFramerate = options.fps;
  codec.active = true;
  codec.numberOfSimulcastStreams = 0;
  codec.SetFrameDropEnabled(false);
  std::string error;
  if (!encoder || encoder->RegisterEncodeCompleteCallback(&collector) != 0 ||
      encoder->InitEncode(
          &codec, VideoEncoder::Settings(VideoEncoder::Capabilities(false), 1,
                                         1200)) != WEBRTC_VIDEO_CODEC_OK ||
      !Identity(*encoder))
    error = "Product factory did not initialize real x265";

  uint64_t warmup_frames = 0;
  const auto warmup_end =
      Clock::now() + std::chrono::seconds(options.warmup_seconds);
  while (error.empty() && Clock::now() < warmup_end) {
    if (!Encode(*encoder, collector, ring, options, warmup_frames, 0,
                warmup_frames % options.key_interval == 0)
             .valid)
      error =
          "Warmup Encode failed or synchronous callback/identity mismatched";
    ++warmup_frames;
  }
  std::vector<Sample> samples;
  std::vector<std::vector<uint8_t>> packets;
  collector.save_packet = !options.bitstream.empty();
  samples.reserve(static_cast<size_t>(options.seconds) * 4096);
  const uint64_t scheduled =
      static_cast<uint64_t>(options.seconds) * options.fps;
  uint64_t slot = 0, skipped = 0, submitted = 0, completed = 0, bytes = 0;
  uint64_t over_budget = 0;
  double last_completion_ms = 0;
  int64_t last_key_period = -1;
  Cpu before, after;
  if (!ReadCpu(&before))
    error = "GetProcessTimes/GetSystemTimes failed";
  const auto start = Clock::now();
  const auto end = start + std::chrono::seconds(options.seconds);
  while (error.empty()) {
    if (options.mode == "sequence" && slot >= ring.size())
      break;
    Clock::time_point planned = start;
    if (options.mode == "paced") {
      if (slot >= scheduled)
        break;
      planned += std::chrono::nanoseconds(slot * 1000000000ull / options.fps);
      std::this_thread::sleep_until(planned);
    }
    const auto frame_start = Clock::now();
    if (options.mode != "sequence" && frame_start >= end) {
      if (options.mode == "paced")
        skipped += scheduled - slot;
      break;
    }
    const int64_t period = static_cast<int64_t>(slot / options.key_interval);
    const bool request_key = period != last_key_period;
    if (request_key)
      last_key_period = period;
    Sample sample = Encode(*encoder, collector, ring, options, slot,
                           warmup_frames, request_key);
    last_completion_ms = Milliseconds(Clock::now() - start).count();
    sample.start_ms = Milliseconds(frame_start - start).count();
    sample.late_ms =
        options.mode == "paced"
            ? std::max(0.0, Milliseconds(frame_start - planned).count())
            : 0;
    ++submitted;
    if (!sample.valid)
      error =
          "Encode failed or callback/frame ID/dimensions/software identity "
          "mismatched";
    else
      ++completed;
    bytes += sample.bytes;
    over_budget += sample.encode_ms > 1000.0 / options.fps;
    samples.push_back(sample);  // Preserve anomalies; no discarded slow frames.
    if (collector.save_packet)
      packets.push_back(std::move(collector.packet));
    ++slot;
    if (options.mode == "paced") {
      const uint64_t future =
          static_cast<uint64_t>(std::floor(
              std::chrono::duration<double>(Clock::now() - start).count() *
              options.fps)) +
          1;
      if (future > slot) {
        const uint64_t next = std::min(future, scheduled);
        skipped += next - slot;
        slot = next;
      }
    }
  }
  if (error.empty() && options.mode == "paced")
    std::this_thread::sleep_until(end);
  if (!ReadCpu(&after))
    error = "Final GetProcessTimes/GetSystemTimes failed";
  const double wall_ms = Milliseconds(Clock::now() - start).count();
  if (completed == 0 && error.empty())
    error = "No successful measured frames";
  const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (logical == 0 && error.empty())
    error = "Unable to read logical processor count";
  const double cpu_ms = after.process >= before.process
                            ? (after.process - before.process) / 10000.0
                            : 0;
  const double core_equivalents = wall_ms > 0 ? cpu_ms / wall_ms : 0;
  const uint64_t system_ticks =
      after.system >= before.system ? after.system - before.system : 0;
  const uint64_t idle_ticks =
      after.idle >= before.idle ? after.idle - before.idle : 0;
  const double system_cpu =
      system_ticks > 0 ? 100.0 * (system_ticks - idle_ticks) / system_ticks : 0;
  DWORD_PTR affinity = 0, system_affinity = 0;
  const bool affinity_known =
      GetProcessAffinityMask(GetCurrentProcess(), &affinity,
                             &system_affinity) != 0;
  if (encoder && encoder->Release() != WEBRTC_VIDEO_CODEC_OK && error.empty())
    error = "Encoder release failed";
  if (!options.bitstream.empty()) {
    std::ofstream stream(options.bitstream, std::ios::binary);
    std::ofstream reference(options.bitstream + ".reference.i420",
                            std::ios::binary);
    if (std::filesystem::exists(options.bitstream + ".keys.json"))
      return 1;
    std::ofstream keys(options.bitstream + ".keys.json");
    keys << "[";
    uint64_t offset = 0;
    bool first = true;
    for (size_t i = 0; i < packets.size(); ++i) {
      stream.write(reinterpret_cast<const char*>(packets[i].data()),
                   packets[i].size());
      const auto& frame = ring[samples[i].slot % ring.size()];
      for (int y = 0; y < options.height; ++y)
        reference.write(reinterpret_cast<const char*>(frame->DataY() +
                                                      y * frame->StrideY()),
                        options.width);
      for (int y = 0; y < options.height / 2; ++y)
        reference.write(reinterpret_cast<const char*>(frame->DataU() +
                                                      y * frame->StrideU()),
                        options.width / 2);
      for (int y = 0; y < options.height / 2; ++y)
        reference.write(reinterpret_cast<const char*>(frame->DataV() +
                                                      y * frame->StrideV()),
                        options.width / 2);
      if (samples[i].key) {
        if (!first)
          keys << ",";
        first = false;
        keys << "{\"frame\":" << i << ",\"offset\":" << offset
             << ",\"bytes\":" << packets[i].size() << "}";
      }
      offset += packets[i].size();
    }
    keys << "]\n";
    stream.close();
    reference.close();
    keys.close();
    if ((!stream || !reference || !keys) && error.empty())
      error = "Quality audit output write failed";
  }
  const std::filesystem::path result_path(options.output);
  if (std::filesystem::exists(result_path)) {
    std::cerr << "Refusing to overwrite measurement\n";
    return 1;
  }
  auto csv_path = result_path;
  csv_path.replace_extension(".csv");
  if (std::filesystem::exists(csv_path))
    return 1;
  std::ofstream csv(csv_path);
  csv << std::fixed << std::setprecision(9)
      << "slot,rtp_timestamp,start_ms,start_lateness_ms,encode_call_ms,bytes,"
         "input_frame_index,"
         "key,valid\n";
  double maximum_late = 0;
  for (const auto& sample : samples) {
    maximum_late = std::max(maximum_late, sample.late_ms);
    csv << sample.slot << ',' << sample.timestamp << ',' << sample.start_ms
        << ',' << sample.late_ms << ',' << sample.encode_ms << ','
        << sample.bytes << ',' << sample.slot % ring.size() << ',' << sample.key
        << ',' << sample.valid << '\n';
  }
  csv.close();
  if (!csv && error.empty())
    error = "Raw CSV write failed";
  std::ofstream output(result_path);
  output << std::fixed << std::setprecision(9)
         << "{\n\"schema_version\":1,\"status\":"
         << Quote(error.empty() ? "MEASURED" : "FAIL")
         << ",\"error\":" << Quote(error)
         << ",\"implementation\":\"x265 H265\",\"hardware\":false"
         << ",\"width\":" << options.width << ",\"height\":" << options.height
         << ",\"target_bitrate_bps\":" << options.bitrate
         << ",\"target_fps\":" << options.fps << ",\"level_id\":" << level
         << ",\"mode\":" << Quote(options.mode)
         << ",\"tuning\":" << Quote(options.tuning)
         << ",\"input_file\":" << Quote(options.input) << ",\"quality_audit\":"
         << (!options.bitstream.empty() ? "true" : "false")
         << ",\"workload\":" << Quote(options.workload) << ",\"generator\":"
         << Quote(options.input.empty()
                      ? "i420-translated-screen-or-texture-v1"
                      : "frozen-real-screen-i420-box-replay-v1")
         << ",\"ring_frames\":" << ring.size()
         << ",\"key_interval_media_frames\":" << options.key_interval
         << ",\"warmup_seconds\":" << options.warmup_seconds
         << ",\"warmup_frames\":" << warmup_frames
         << ",\"measurement_seconds_requested\":" << options.seconds
         << ",\"wall_ms\":" << wall_ms << ",\"process_cpu_ms\":" << cpu_ms
         << ",\"logical_processors\":" << logical << ",\"process_cpu_percent\":"
         << (logical ? 100.0 * core_equivalents / logical : 0)
         << ",\"process_core_equivalents\":" << core_equivalents
         << ",\"system_cpu_percent\":" << system_cpu
         << ",\"affinity_known\":" << (affinity_known ? "true" : "false")
         << ",\"process_affinity_mask\":" << static_cast<uint64_t>(affinity)
         << ",\"system_affinity_mask\":"
         << static_cast<uint64_t>(system_affinity) << ",\"completed_fps\":"
         << (wall_ms > 0 ? completed * 1000.0 / wall_ms : 0)
         << ",\"submitted_frames\":" << submitted
         << ",\"completed_frames\":" << completed
         << ",\"scheduled_slots\":" << (options.mode == "paced" ? scheduled : 0)
         << ",\"skipped_slots\":" << skipped << ",\"encoded_bytes\":" << bytes
         << ",\"encode_call_over_budget\":" << over_budget
         << ",\"maximum_start_lateness_ms\":" << maximum_late
         << ",\"final_completion_overrun_ms\":"
         << std::max(0.0, last_completion_ms - options.seconds * 1000.0)
         << ",\"wall_span_overrun_ms\":"
         << std::max(0.0, wall_ms - options.seconds * 1000.0)
         << ",\"frame_budget_ms\":" << 1000.0 / options.fps
         << ",\"configuration\":"
         << Quote(
                "ultrafast/zerolatency/frameThreads1/noB/"
                "noLookahead;experimental=" +
                options.tuning)
         << ",\"latency_scope\":\"complete synchronous Encode including "
            "adapter conversion/backend/AU validation/light callback; frame "
            "generation/init/warmup/IO excluded\""
         << ",\"cpu_scope\":\"process user+kernel over complete measurement "
            "wall span, including pacing wait and light probe overhead; "
            "normalized to all logical processors\""
         << ",\"scheduler\":\"absolute slots with overdue slots skipped; no "
            "burst catchup; std::this_thread::sleep_until without OS "
            "timer/priority tuning\""
         << ",\"raw_csv\":" << Quote(csv_path.filename().string())
         << ",\"encode_call_ms\":";
  Distribution(output, samples, 0);
  output << ",\"key_encode_call_ms\":";
  Distribution(output, samples, 1);
  output << ",\"delta_encode_call_ms\":";
  Distribution(output, samples, 2);
  output << "\n}\n";
  output.close();
  if (!output)
    return 1;
  std::cout << (error.empty() ? "MEASURED" : "FAIL")
            << " fps=" << (wall_ms > 0 ? completed * 1000.0 / wall_ms : 0)
            << " output=" << result_path.string() << '\n';
  return error.empty() ? 0 : 1;
}
}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  webrtc::Options options;
  if (!webrtc::Parse(argc, argv, &options)) {
    std::cerr << "Invalid bounded probe arguments\n";
    return 1;
  }
  webrtc::LogMessage::LogToDebug(webrtc::LS_ERROR);
  return webrtc::Run(options);
}
