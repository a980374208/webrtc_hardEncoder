/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
// Test-only input capture. No encoder is created. All captured pixels stay
// local. Capture the actual primary desktop once, then compare encoder
// configurations by replaying the immutable packed I420 sequence. Disk I/O and
// fingerprints are deferred until capture and same-thread capturer teardown are
// complete.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/scale.h"

namespace webrtc {
namespace {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr size_t kYBytes = static_cast<size_t>(kWidth) * kHeight;
constexpr size_t kFrameBytes = kYBytes * 3 / 2;

struct Options {
  std::string output;
  int seconds = 6;
  int fps = 30;
};

struct Sample {
  uint64_t slot = 0;
  double start_ms = 0;
  double late_ms = 0;
  double capture_ms = 0;
  double conversion_ms = 0;
  int callbacks = 0;
  int capture_result = -1;
  int source_width = 0;
  int source_height = 0;
  uint32_t capturer_id = 0;
  int content_width = 0;
  int content_height = 0;
  int content_x = 0;
  int content_y = 0;
  bool changed = false;
  bool valid = false;
  uint64_t fingerprint = 0;
  std::vector<uint8_t> pixels;
};

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

bool Parse(int argc, char** argv, Options* options) {
  if (argc % 2 != 1)
    return false;
  for (int i = 1; i < argc; i += 2) {
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--output") {
      options->output = value;
      continue;
    }
    char* end = nullptr;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || !end || *end || number < 1 || number > 180)
      return false;
    if (key == "--seconds")
      options->seconds = static_cast<int>(number);
    else if (key == "--fps")
      options->fps = static_cast<int>(number);
    else
      return false;
  }
  return !options->output.empty() &&
         std::filesystem::path(options->output).is_absolute() &&
         options->seconds <= 6 && options->fps <= 30 &&
         options->seconds * options->fps <= 180;
}

class Collector final : public DesktopCapturer::Callback {
 public:
  void Reset() {
    callbacks = 0;
    result = DesktopCapturer::Result::ERROR_PERMANENT;
    frame.reset();
  }
  void OnCaptureResult(DesktopCapturer::Result value,
                       std::unique_ptr<DesktopFrame> image) override {
    ++callbacks;
    result = value;
    frame = std::move(image);
  }
  int callbacks = 0;
  DesktopCapturer::Result result = DesktopCapturer::Result::ERROR_PERMANENT;
  std::unique_ptr<DesktopFrame> frame;
};

// GDI uses the EnumDisplayDevicesW device index as SourceId. Match the primary
// monitor's device name as well as the display's PRIMARY_DEVICE flag rather
// than assuming that the first enumerated source is primary.
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

bool Convert(const DesktopFrame& source, Sample* sample) {
  const int width = source.size().width(), height = source.size().height();
  if (!source.data() || width < 2 || height < 2 || width > 16384 ||
      height > 16384 || source.stride() < width * 4)
    return false;
  sample->source_width = width;
  sample->source_height = height;
  sample->capturer_id = source.capturer_id();
  auto native = I420Buffer::Create(width, height);
  if (libyuv::ARGBToI420(source.data(), source.stride(), native->MutableDataY(),
                         native->StrideY(), native->MutableDataU(),
                         native->StrideU(), native->MutableDataV(),
                         native->StrideV(), width, height) != 0)
    return false;

  // Preserve the entire primary screen. Round the fitted dimensions and
  // centered offsets down to even coordinates for I420 chroma alignment.
  int fitted_width = kWidth, fitted_height = kHeight;
  if (static_cast<int64_t>(width) * kHeight >
      static_cast<int64_t>(height) * kWidth) {
    fitted_height =
        static_cast<int>(static_cast<int64_t>(height) * kWidth / width) & ~1;
  } else {
    fitted_width =
        static_cast<int>(static_cast<int64_t>(width) * kHeight / height) & ~1;
  }
  if (fitted_width < 2 || fitted_height < 2)
    return false;
  sample->content_width = fitted_width;
  sample->content_height = fitted_height;
  sample->content_x = ((kWidth - fitted_width) / 2) & ~1;
  sample->content_y = ((kHeight - fitted_height) / 2) & ~1;
  sample->pixels.assign(kFrameBytes, 128);
  std::fill_n(sample->pixels.data(), kYBytes, 16);
  uint8_t* const y = sample->pixels.data();
  uint8_t* const u = y + kYBytes;
  uint8_t* const v = u + kYBytes / 4;
  return libyuv::I420Scale(
             native->DataY(), native->StrideY(), native->DataU(),
             native->StrideU(), native->DataV(), native->StrideV(), width,
             height, y + sample->content_y * kWidth + sample->content_x, kWidth,
             u + (sample->content_y / 2) * (kWidth / 2) + sample->content_x / 2,
             kWidth / 2,
             v + (sample->content_y / 2) * (kWidth / 2) + sample->content_x / 2,
             kWidth / 2, fitted_width, fitted_height, libyuv::kFilterBox) == 0;
}

uint64_t Fingerprint(const std::vector<uint8_t>& pixels) {
  uint64_t value = 14695981039346656037ULL;
  for (uint8_t byte : pixels) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}

int Run(int argc, char** argv) {
  Options options;
  if (!Parse(argc, argv, &options)) {
    std::cerr << "Usage: x265_screen_sequence_capture --output ABSOLUTE_PREFIX "
                 "[--seconds 1..6] [--fps 1..30]\n";
    return 2;
  }
  LogMessage::LogToDebug(LS_WARNING);
  const std::filesystem::path prefix(options.output);
  const std::filesystem::path raw_path(options.output + ".i420");
  const std::filesystem::path csv_path(options.output + ".frames.csv");
  const std::filesystem::path json_path(options.output + ".metadata.json");
  if (std::filesystem::exists(raw_path) || std::filesystem::exists(csv_path) ||
      std::filesystem::exists(json_path)) {
    std::cerr << "FAIL output already exists; preserve the previous capture\n";
    return 2;
  }
  std::filesystem::create_directories(prefix.parent_path());
  Collector collector;
  auto capture_options = DesktopCaptureOptions::CreateDefault();
  capture_options.set_allow_directx_capturer(false);
  capture_options.set_disable_effects(false);
  capture_options.set_detect_updated_region(false);
  capture_options.set_prefer_cursor_embedded(false);
#if defined(RTC_ENABLE_WIN_WGC)
  capture_options.set_allow_wgc_screen_capturer(false);
#endif
  auto capturer = DesktopCapturer::CreateScreenCapturer(capture_options);
  DesktopCapturer::SourceId source_id = -2;
  std::string device_name, failure;
  RECT monitor_rect{};
  if (!capturer ||
      !FindPrimary(*capturer, &source_id, &device_name, &monitor_rect))
    failure = "primary GDI source could not be selected reliably";
  if (failure.empty())
    capturer->Start(&collector);
  const uint64_t scheduled =
      static_cast<uint64_t>(options.seconds) * options.fps;
  uint64_t skipped = 0;
  std::vector<Sample> samples;
  samples.reserve(static_cast<size_t>(scheduled));
  const auto start = Clock::now();
  const auto finish = start + std::chrono::seconds(options.seconds);
  uint64_t slot = 0;
  while (failure.empty() && slot < scheduled) {
    auto now = Clock::now();
    if (now >= finish) {
      skipped += scheduled - slot;
      break;
    }
    const auto elapsed = std::chrono::duration<double>(now - start).count();
    const auto due = static_cast<uint64_t>(elapsed * options.fps);
    if (due > slot) {
      skipped += std::min(due, scheduled) - slot;
      slot = std::min(due, scheduled);
      if (slot >= scheduled)
        break;
    }
    auto deadline =
        start + std::chrono::nanoseconds(slot * 1000000000ULL / options.fps);
    if (now < deadline)
      std::this_thread::sleep_until(deadline);
    const auto capture_start = Clock::now();
    if (capture_start >= finish) {
      skipped += scheduled - slot;
      break;
    }
    // Skip stale slots after a long scheduler stall instead of catching up
    // with burst captures. The current slot is always the latest due one.
    const auto wake_due = static_cast<uint64_t>(
        std::chrono::duration<double>(capture_start - start).count() *
        options.fps);
    if (wake_due > slot) {
      skipped += wake_due - slot;
      slot = wake_due;
      deadline =
          start + std::chrono::nanoseconds(slot * 1000000000ULL / options.fps);
    }
    Sample sample;
    sample.slot = slot;
    sample.start_ms = Milliseconds(capture_start - start).count();
    sample.late_ms =
        std::max(0.0, Milliseconds(capture_start - deadline).count());
    collector.Reset();
    capturer->CaptureFrame();
    const auto capture_end = Clock::now();
    sample.capture_ms = Milliseconds(capture_end - capture_start).count();
    sample.callbacks = collector.callbacks;
    sample.capture_result = static_cast<int>(collector.result);
    if (collector.callbacks != 1 ||
        collector.result != DesktopCapturer::Result::SUCCESS ||
        !collector.frame) {
      failure =
          "capture failed or did not synchronously return exactly one frame";
    } else if (collector.frame->capturer_id() !=
               DesktopCapturerId::kScreenCapturerWinGdi) {
      failure = "actual capturer is not the selected GDI implementation";
    } else {
      const auto conversion_start = Clock::now();
      sample.valid = Convert(*collector.frame, &sample);
      sample.conversion_ms =
          Milliseconds(Clock::now() - conversion_start).count();
      if (!sample.valid)
        failure = "BGRA conversion or aspect-preserving I420 scaling failed";
      else if (!samples.empty() &&
               (sample.source_width != samples.front().source_width ||
                sample.source_height != samples.front().source_height)) {
        sample.valid = false;
        failure = "primary desktop dimensions changed during capture";
      }
    }
    samples.push_back(std::move(sample));
    ++slot;
  }
  if (failure.empty() && Clock::now() < finish)
    std::this_thread::sleep_until(finish);
  const double wall_seconds =
      std::chrono::duration<double>(Clock::now() - start).count();
  // Frame and capturer destruction precede all payload I/O on this owner
  // thread.
  collector.frame.reset();
  capturer.reset();

  std::ofstream raw(raw_path, std::ios::binary);
  std::ofstream csv(csv_path);
  uint64_t frame_count = 0, changed_frames = 0;
  const std::vector<uint8_t>* previous = nullptr;
  csv << "attempt,slot,start_ms,start_lateness_ms,capture_ms,conversion_ms,"
         "callbacks,capture_result,source_width,source_height,capturer_id,"
         "content_x,content_y,content_width,content_height,valid,payload_bytes,"
         "changed_from_previous,fnv1a64\n";
  csv << std::setprecision(10);
  for (size_t i = 0; i < samples.size(); ++i) {
    auto& sample = samples[i];
    if (sample.valid) {
      sample.fingerprint = Fingerprint(sample.pixels);
      sample.changed =
          previous &&
          std::memcmp(previous->data(), sample.pixels.data(), kFrameBytes) != 0;
      changed_frames += sample.changed;
      raw.write(reinterpret_cast<const char*>(sample.pixels.data()),
                static_cast<std::streamsize>(sample.pixels.size()));
      previous = &sample.pixels;
      ++frame_count;
    }
    csv << i << ',' << sample.slot << ',' << sample.start_ms << ','
        << sample.late_ms << ',' << sample.capture_ms << ','
        << sample.conversion_ms << ',' << sample.callbacks << ','
        << sample.capture_result << ',' << sample.source_width << ','
        << sample.source_height << ',' << sample.capturer_id << ','
        << sample.content_x << ',' << sample.content_y << ','
        << sample.content_width << ',' << sample.content_height << ','
        << sample.valid << ',' << (sample.valid ? sample.pixels.size() : 0)
        << ',' << sample.changed << ',' << std::hex << std::setw(16)
        << std::setfill('0') << sample.fingerprint << std::dec
        << std::setfill(' ') << '\n';
  }
  raw.close();
  csv.close();
  if (!raw || !csv)
    failure = "capture payload or CSV write failed";
  if (frame_count == 0 && failure.empty())
    failure = "capture produced no usable frames";
  std::ofstream metadata(json_path);
  metadata
      << std::setprecision(10) << "{\n\"schema_version\":1,\"status\":"
      << Quote(failure.empty() ? "CAPTURED" : "FAIL")
      << ",\"failure\":" << Quote(failure)
      << ",\"content_kind\":\"actual_current_primary_desktop\""
      << ",\"source_selection\":\"GDI device index matched to primary "
         "MONITORINFOEXW.szDevice and DISPLAY_DEVICE_PRIMARY_DEVICE\""
      << ",\"source_id\":" << source_id
      << ",\"source_device_name\":" << Quote(device_name)
      << ",\"monitor_rect\":{\"left\":" << monitor_rect.left
      << ",\"top\":" << monitor_rect.top << ",\"right\":" << monitor_rect.right
      << ",\"bottom\":" << monitor_rect.bottom << '}' << ",\"width\":" << kWidth
      << ",\"height\":" << kHeight
      << ",\"pixel_format\":\"I420\",\"frame_bytes\":" << kFrameBytes
      << ",\"requested_seconds\":" << options.seconds
      << ",\"requested_fps\":" << options.fps
      << ",\"actual_wall_seconds\":" << wall_seconds
      << ",\"scheduled_slots\":" << scheduled
      << ",\"capture_attempts\":" << samples.size()
      << ",\"frame_count\":" << frame_count << ",\"skipped_slots\":" << skipped
      << ",\"unattempted_slots_after_failure\":"
      << scheduled - samples.size() - skipped
      << ",\"consecutive_changed_frames\":" << changed_frames
      << ",\"dynamic_content_observed\":"
      << (changed_frames > 0 ? "true" : "false")
      << ",\"payload_bytes\":" << frame_count * kFrameBytes
      << ",\"raw_file\":" << Quote(raw_path.filename().string())
      << ",\"frames_csv\":" << Quote(csv_path.filename().string())
      << ",\"conversion\":\"libyuv ARGBToI420 (BGRA memory), I420Scale "
         "kFilterBox; fit full source preserving aspect ratio, round fit "
         "and centered offsets down to even values; letterbox Y16 U128 V128\""
      << ",\"fingerprint_algorithm\":\"FNV-1a-64, complete packed I420; "
         "not a cryptographic hash; runner must bind full-file SHA256\""
      << ",\"timing_scope\":\"capture_ms covers CaptureFrame plus callback; "
         "conversion_ms covers owned native I420 plus scale plus output "
         "allocation; fingerprints, comparisons and all disk I/O follow "
         "the capture window\"}\n";
  metadata.close();
  if (!metadata) {
    std::cerr << "FAIL metadata write failed\n";
    return 1;
  }
  std::cout << (failure.empty() ? "CAPTURED" : "FAIL")
            << " frames=" << frame_count << " skipped_slots=" << skipped
            << " changed_frames=" << changed_frames
            << " metadata=" << json_path.string() << '\n';
  return failure.empty() ? 0 : 1;
}
}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  return webrtc::Run(argc, argv);
}
