/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

// Local codec/RTP smoke, not a PeerConnection/network or RTCP PLI test.
// Compile with RTC_QSV_H264_SMOKE to validate QSV using the same H264 gates.
// --software validates the harness with OpenH264, not hardware acceptance.
#include <algorithm>
#include <cmath>
#include <cstdint>
#if defined(RTC_QSV_H264_SMOKE)
#include <cstdio>
#include <cwchar>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Windows types must precede the DbgHelp declarations.
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#endif
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/environment/environment_factory.h"
#if defined(RTC_QSV_H264_SMOKE) || defined(RTC_USE_QSV) || \
    defined(RTC_AMF_H264_SMOKE)
#include "api/field_trials.h"
#endif
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_common.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/rtp_rtcp/source/rtp_format_h264.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer_h264.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#if defined(RTC_QSV_H264_SMOKE)
#include "modules/video_coding/codecs/external/qsv_video_encoder_backend.h"  // nogncheck
#elif defined(RTC_AMF_H264_SMOKE)
#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"  // nogncheck
#else
#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend.h"  // nogncheck
#endif
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/h264_sps_pps_tracker.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr double kMinimumPsnr = 28.0;

#if defined(RTC_QSV_H264_SMOKE)
constexpr char kHardwareName[] = "QSV";
constexpr char kSmokeName[] = "qsv_h264_smoke";
constexpr char kRequireHardwareArgument[] = "--require-qsv";

// Diagnostic-only, process-local capture. Keep DbgHelp loaded until process
// exit and prepare the path before entering a possibly corrupt vendor stack.
wchar_t g_qsv_dump_path[MAX_PATH] = {};
decltype(&MiniDumpWriteDump) g_qsv_dump_writer = nullptr;
volatile LONG g_qsv_dump_started = 0;

LONG WINAPI CaptureQsvUnhandledException(EXCEPTION_POINTERS* pointers) {
  if (InterlockedExchange(&g_qsv_dump_started, 1) != 0)
    return EXCEPTION_CONTINUE_SEARCH;
  BOOL written = FALSE;
  DWORD dump_error = ERROR_SUCCESS;
  HANDLE file =
      CreateFileW(g_qsv_dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION exception;
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = pointers;
    exception.ClientPointers = FALSE;
    written = g_qsv_dump_writer(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                   MiniDumpWithUnloadedModules),
        &exception, nullptr, nullptr);
    if (!written)
      dump_error = GetLastError();
    FlushFileBuffers(file);
    CloseHandle(file);
  } else {
    dump_error = GetLastError();
  }
  // Avoid iostream allocation/locking while handling a possibly damaged heap.
  char message[256];
  const int length = std::snprintf(
      message, sizeof(message),
      "QSV_UNHANDLED_EXCEPTION code=0x%08lx address=%p thread=%lu "
      "dump_written=%d dump_error=%lu\n",
      pointers && pointers->ExceptionRecord
          ? pointers->ExceptionRecord->ExceptionCode
          : 0UL,
      pointers && pointers->ExceptionRecord
          ? pointers->ExceptionRecord->ExceptionAddress
          : nullptr,
      GetCurrentThreadId(), written, dump_error);
  if (length > 0 && length < static_cast<int>(sizeof(message))) {
    DWORD bytes_written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, length, &bytes_written,
              nullptr);
  }
  // Preserve the original exception and crash exit code. A dump is not a PASS.
  return EXCEPTION_CONTINUE_SEARCH;
}

bool InstallQsvCrashDiagnostics(const std::string& directory) {
  wchar_t converted[MAX_PATH];
  wchar_t absolute[MAX_PATH];
  if (!MultiByteToWideChar(CP_ACP, 0, directory.c_str(), -1, converted,
                           MAX_PATH))
    return false;
  const DWORD path_length =
      GetFullPathNameW(converted, MAX_PATH, absolute, nullptr);
  if (path_length == 0 || path_length >= MAX_PATH)
    return false;
  const DWORD attributes = GetFileAttributesW(absolute);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    return false;
  const int length =
      std::swprintf(g_qsv_dump_path, MAX_PATH, L"%ls\\qsv_h264_smoke-%lu.dmp",
                    absolute, GetCurrentProcessId());
  if (length <= 0 || length >= MAX_PATH)
    return false;
  HMODULE dbghelp =
      LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dbghelp)
    return false;
  g_qsv_dump_writer = reinterpret_cast<decltype(g_qsv_dump_writer)>(
      GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  if (!g_qsv_dump_writer) {
    FreeLibrary(dbghelp);
    return false;
  }
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  SetUnhandledExceptionFilter(CaptureQsvUnhandledException);
  std::cerr << "DIAGNOSTIC minidump enabled directory=" << directory << '\n';
  return true;
}
#elif defined(RTC_AMF_H264_SMOKE)
constexpr char kHardwareName[] = "AMF";
constexpr char kSmokeName[] = "amf_h264_smoke";
constexpr char kRequireHardwareArgument[] = "--require-amf";
#else
constexpr char kHardwareName[] = "NVENC";
constexpr char kSmokeName[] = "nvenc_h264_smoke";
constexpr char kRequireHardwareArgument[] = "--require-nvenc";
#endif

external_encoder::Status ProbeHardwareBackend(std::string* reason) {
#if defined(RTC_QSV_H264_SMOKE)
  return external_encoder::ProbeQsvH264Backend(reason);
#elif defined(RTC_AMF_H264_SMOKE)
  return external_encoder::ProbeAmfH264Backend(reason);
#else
  return external_encoder::ProbeNvencH264Backend(reason);
#endif
}

std::unique_ptr<external_encoder::VideoEncoderBackend> CreateHardwareBackend() {
#if defined(RTC_QSV_H264_SMOKE)
  return external_encoder::CreateQsvH264Backend();
#elif defined(RTC_AMF_H264_SMOKE)
  return external_encoder::CreateAmfH264Backend();
#else
  return external_encoder::CreateNvencH264Backend();
#endif
}

bool Check(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL " << message << '\n';
  return condition;
}

SdpVideoFormat Format() {
  return CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel3_1,
                          "1");
}

VideoCodec CodecSettings() {
  VideoCodec codec;
  codec.codecType = kVideoCodecH264;
  codec.width = kWidth;
  codec.height = kHeight;
  codec.startBitrate = 600;
  codec.minBitrate = 30;
  codec.maxBitrate = 1500;
  codec.maxFramerate = 30;
  codec.active = true;
  codec.numberOfSimulcastStreams = 0;
  codec.SetFrameDropEnabled(false);
  *codec.H264() = VideoEncoder::GetDefaultH264Settings();
  codec.H264()->numberOfTemporalLayers = 1;
  codec.H264()->keyFrameInterval = 300;
  return codec;
}

VideoFrame MakeFrame(int index, uint32_t timestamp) {
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
  return VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_rtp_timestamp(timestamp)
      .set_timestamp_us(static_cast<int64_t>(timestamp) * 1000000 / 90000)
      .build();
}

bool HasNal(const EncodedImage& image, H264::NaluType type) {
  for (const auto& nalu : H264::FindNaluIndices(
           ArrayView<const uint8_t>(image.data(), image.size()))) {
    if (nalu.payload_size > 0 &&
        H264::ParseNaluType(image.data()[nalu.payload_start_offset]) == type)
      return true;
  }
  return false;
}

class EncodeCollector final : public EncodedImageCallback {
 public:
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* info) override {
    images.push_back(image);
    if (info)
      infos.push_back(*info);
    return Result(Result::OK, image.RtpTimestamp());
  }
  void Clear() {
    images.clear();
    infos.clear();
  }
  std::vector<EncodedImage> images;
  std::vector<CodecSpecificInfo> infos;
};

class DecodeCollector final : public DecodedImageCallback {
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

class RoundTrip {
 public:
  ~RoundTrip() {
    if (decoder_)
      decoder_->Release();
  }

  bool ResetDecoder() {
    if (decoder_)
      decoder_->Release();
    decoder_ = H264Decoder::Create();
    tracker_ = std::make_unique<video_coding::H264SpsPpsTracker>();
    if (!Check(decoder_ != nullptr, "H264 decoder creation"))
      return false;
    VideoDecoder::Settings settings;
    settings.set_codec_type(kVideoCodecH264);
    settings.set_max_render_resolution({kWidth, kHeight});
    return Check(decoder_->Configure(settings), "FFmpeg configuration") &&
           Check(decoder_->RegisterDecodeCompleteCallback(&decoded_) ==
                     WEBRTC_VIDEO_CODEC_OK,
                 "decode callback registration") &&
           Check(std::string(decoder_->ImplementationName()) == "FFmpeg",
                 "decoder implementation identity");
  }

  bool Decode(const EncodedImage& image,
              const CodecSpecificInfo& info,
              const VideoFrame& input) {
    if (!Check(info.codecType == kVideoCodecH264 &&
                   info.codecSpecific.H264.packetization_mode ==
                       H264PacketizationMode::NonInterleaved,
               "H264 packetization metadata"))
      return false;
    RtpPacketizer::PayloadSizeLimits limits;
    limits.max_payload_len = 1200;
    RtpPacketizerH264 packetizer(
        ArrayView<const uint8_t>(image.data(), image.size()), limits,
        H264PacketizationMode::NonInterleaved);
    const size_t expected_packets = packetizer.NumPackets();
    if (!Check(expected_packets != 0, "RTP packetization"))
      return false;
    std::vector<CopyOnWriteBuffer> fixed_payloads;
    size_t markers = 0;
    for (size_t i = 0; i < expected_packets; ++i) {
      RtpPacketToSend packet(nullptr);
      packet.SetTimestamp(image.RtpTimestamp());
      packet.SetSequenceNumber(sequence_++);
      packet.SetSsrc(0x12345678);
      packet.SetPayloadType(100);
      if (!Check(packetizer.NextPacket(&packet), "RTP next packet"))
        return false;
      RtpPacketReceived received;
      if (!Check(received.Parse(packet.data(), packet.size()),
                 "serialized RTP parse") ||
          !Check(received.Timestamp() == image.RtpTimestamp() &&
                     received.SequenceNumber() == packet.SequenceNumber(),
                 "serialized RTP metadata") ||
          !Check(received.payload_size() <= 1200, "RTP payload size"))
        return false;
      markers += received.Marker() ? 1 : 0;
      if (!Check(received.Marker() == (i + 1 == expected_packets),
                 "RTP final marker"))
        return false;
      const uint8_t nal_type = received.payload()[0] & 0x1f;
      fu_a_ += nal_type == H264::kFuA;
      stap_a_ += nal_type == H264::kStapA;
      auto parsed = depacketizer_.Parse(CopyOnWriteBuffer(received.payload()));
      if (!Check(parsed.has_value(), "H264 depacketization"))
        return false;
      auto fixed = tracker_->CopyAndFixBitstream(
          ArrayView<const uint8_t>(parsed->video_payload),
          &parsed->video_header);
      if (!Check(fixed.action == video_coding::H264SpsPpsTracker::kInsert,
                 "H264 SPS/PPS and Annex B reconstruction"))
        return false;
      fixed_payloads.push_back(std::move(fixed.bitstream));
    }
    if (!Check(markers == 1 && packetizer.NumPackets() == 0,
               "RTP frame completion"))
      return false;
    std::vector<ArrayView<const uint8_t>> fragments;
    for (const auto& payload : fixed_payloads)
      fragments.emplace_back(payload.data(), payload.size());
    EncodedImage assembled = image;
    assembled.SetEncodedData(depacketizer_.AssembleFrame(fragments));
    decoded_.frames.clear();
    if (!Check(decoder_->Decode(assembled, false, -1) == WEBRTC_VIDEO_CODEC_OK,
               "FFmpeg decode") ||
        !Check(decoded_.frames.size() == 1, "decoded frame count"))
      return false;
    const VideoFrame& output = decoded_.frames[0];
    if (!Check(output.width() == kWidth && output.height() == kHeight,
               "decoded dimensions") ||
        !Check(output.rtp_timestamp() == input.rtp_timestamp(),
               "decoded RTP timestamp"))
      return false;
    const double psnr = I420PSNR(&input, &output);
    if (!Check(std::isfinite(psnr) && psnr >= kMinimumPsnr,
               "decoded content PSNR >= 28 dB")) {
      std::cerr << "observed_psnr=" << psnr << '\n';
      return false;
    }
    minimum_psnr_ = std::min(minimum_psnr_, psnr);
    ++frames_;
    bytes_ += image.size();
    return true;
  }

  void Print(const std::string& identity) const {
    std::cout << "PASS local_h264_rtp_decode implementation=" << identity
              << " frames=" << frames_ << " encoded_bytes=" << bytes_
              << " fu_a_packets=" << fu_a_ << " stap_a_packets=" << stap_a_
              << " minimum_psnr_db=" << minimum_psnr_ << '\n';
  }
  bool SawFragmentation() const { return fu_a_ != 0; }

 private:
  std::unique_ptr<H264Decoder> decoder_;
  DecodeCollector decoded_;
  VideoRtpDepacketizerH264 depacketizer_;
  std::unique_ptr<video_coding::H264SpsPpsTracker> tracker_;
  uint16_t sequence_ = 1;
  size_t frames_ = 0;
  size_t bytes_ = 0;
  size_t fu_a_ = 0;
  size_t stap_a_ = 0;
  double minimum_psnr_ = 48;
};

void SetRates(VideoEncoder& encoder, uint32_t bps, double fps) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, bps);
  encoder.SetRates(VideoEncoder::RateControlParameters(allocation, fps));
}

bool EncodeAndDecode(VideoEncoder& encoder,
                     EncodeCollector& collector,
                     RoundTrip& round_trip,
                     const VideoFrame& input,
                     bool force_key,
                     bool require_key) {
  collector.Clear();
  const std::vector<VideoFrameType> key = {VideoFrameType::kVideoFrameKey};
  if (!Check(encoder.Encode(input, force_key ? &key : nullptr) ==
                 WEBRTC_VIDEO_CODEC_OK,
             "encode return status") ||
      !Check(collector.images.size() == 1 && collector.infos.size() == 1,
             "synchronous complete access unit"))
    return false;
  const auto& image = collector.images[0];
  const auto& info = collector.infos[0];
  if (!Check(image.RtpTimestamp() == input.rtp_timestamp(),
             "encoded RTP timestamp") ||
      !Check(image._encodedWidth == kWidth && image._encodedHeight == kHeight,
             "encoded dimensions"))
    return false;
  const bool idr = HasNal(image, H264::kIdr);
  if (!Check((image._frameType == VideoFrameType::kVideoFrameKey) == idr &&
                 info.codecSpecific.H264.idr_frame == idr,
             "real IDR matches callback metadata"))
    return false;
  if (require_key &&
      (!Check(idr && HasNal(image, H264::kSps) && HasNal(image, H264::kPps),
              "requested/resumed IDR with in-band SPS/PPS") ||
       !round_trip.ResetDecoder()))
    return false;
  return round_trip.Decode(image, info, input);
}

bool RunSequence(VideoEncoder& encoder, bool hardware) {
  EncodeCollector collector;
  RoundTrip round_trip;
  const auto codec = CodecSettings();
  uint32_t timestamp = 90000;
  for (int session = 0; session < 2; ++session) {
    if (!Check(encoder.RegisterEncodeCompleteCallback(&collector) ==
                   WEBRTC_VIDEO_CODEC_OK,
               "encode callback registration") ||
        !Check(encoder.InitEncode(
                   &codec, VideoEncoder::Settings(
                               VideoEncoder::Capabilities(false), 1, 1200)) ==
                   WEBRTC_VIDEO_CODEC_OK,
               "encoder initialization"))
      return false;
    auto identity = encoder.GetEncoderInfo();
    if (!Check(identity.is_hardware_accelerated == hardware,
               "encoder hardware identity") ||
        (hardware && !Check(identity.implementation_name.find(kHardwareName) !=
                                std::string::npos,
                            "requested hardware implementation identity")))
      return false;
    for (int i = 0; i < 30; ++i) {
      if (i == 15)
        SetRates(encoder, 250000, 15);
      if (i == 20) {
        SetRates(encoder, 0, 15);
        for (int paused = 0; paused < 3; ++paused) {
          collector.Clear();
          timestamp += 6000;
          if (!Check(encoder.Encode(MakeFrame(100 + paused, timestamp),
                                    nullptr) == WEBRTC_VIDEO_CODEC_OK,
                     "paused encode return status") ||
              !Check(collector.images.empty(), "paused output suppression"))
            return false;
        }
        SetRates(encoder, 350000, 15);
      }
      timestamp += i >= 15 ? 6000 : 3000;
      const auto frame = MakeFrame(session * 30 + i, timestamp);
      if (!EncodeAndDecode(encoder, collector, round_trip, frame, i == 10,
                           i == 0 || i == 10 || i == 20))
        return false;
      if (!Check(encoder.GetEncoderInfo().is_hardware_accelerated == hardware,
                 "encoder remained on requested implementation"))
        return false;
    }
    if (!Check(encoder.Release() == WEBRTC_VIDEO_CODEC_OK, "encoder release") ||
        !Check(encoder.Release() == WEBRTC_VIDEO_CODEC_OK,
               "idempotent encoder release"))
      return false;
  }
  if (!Check(round_trip.SawFragmentation(), "RTP FU-A exercised"))
    return false;
  round_trip.Print(hardware ? kHardwareName : "OpenH264 software harness");
  std::cout << "PASS key_request rates_change pause_resume restart\n";
  return true;
}

bool CheckBuiltinFactory(bool hardware) {
  auto factory = CreateBuiltinVideoEncoderFactory();
#if defined(RTC_QSV_H264_SMOKE) || defined(RTC_USE_QSV) || \
    defined(RTC_AMF_H264_SMOKE)
  // A vendor-specific smoke must not silently select another hardware vendor
  // when both backends are enabled. This is scoped to the created Environment.
  const auto environment = CreateEnvironment(std::make_unique<FieldTrials>(
      std::string("WebRTC-H264-HardwareEncoder/") + kHardwareName + "/"));
#else
  const auto environment = CreateEnvironment();
#endif
  auto encoder = factory->Create(environment, Format());
  if (!Check(encoder != nullptr, "builtin factory creation"))
    return false;
  EncodeCollector collector;
  RoundTrip round_trip;
  const auto codec = CodecSettings();
  if (!Check(encoder->RegisterEncodeCompleteCallback(&collector) ==
                 WEBRTC_VIDEO_CODEC_OK,
             "builtin callback registration") ||
      !Check(encoder->InitEncode(&codec, VideoEncoder::Settings(
                                             VideoEncoder::Capabilities(false),
                                             1, 1200)) == WEBRTC_VIDEO_CODEC_OK,
             "builtin encoder initialization"))
    return false;
  const auto info = encoder->GetEncoderInfo();
  if (!Check(info.is_hardware_accelerated == hardware,
             "builtin implementation hardware identity") ||
      (hardware &&
       !Check(info.implementation_name.find(kHardwareName) != std::string::npos,
              "builtin selected requested hardware vendor")))
    return false;
  if (!EncodeAndDecode(*encoder, collector, round_trip, MakeFrame(1, 90000),
                       true, true))
    return false;
  const bool stayed_hardware =
      encoder->GetEncoderInfo().is_hardware_accelerated;
  if (!Check(stayed_hardware == hardware,
             "builtin implementation after encode"))
    return false;
  encoder->Release();
  std::cout << "PASS builtin_factory implementation="
            << info.implementation_name << " hardware=" << hardware << '\n';
  return true;
}

int RunSmoke(int argc, char** argv) {
  bool require_hardware = false;
  bool software = false;
#if defined(RTC_QSV_H264_SMOKE)
  std::string diagnostic_directory;
  bool diagnostic_selftest = false;
#endif
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument == kRequireHardwareArgument) {
      require_hardware = true;
    } else if (argument == "--software") {
      software = true;
#if defined(RTC_QSV_H264_SMOKE)
    } else if (argument == "--diagnostic-dir" && i + 1 < argc) {
      diagnostic_directory = argv[++i];
      if (!Check(!diagnostic_directory.empty(), "empty diagnostic directory"))
        return 1;
    } else if (argument == "--diagnostic-selftest") {
      diagnostic_selftest = true;
#endif
    } else {
      std::cerr << "Usage: " << kSmokeName << " [" << kRequireHardwareArgument
                << " | --software]"
#if defined(RTC_QSV_H264_SMOKE)
                << " [--diagnostic-dir <existing local directory>]"
                   " [--diagnostic-selftest (diagnostics only)]"
#endif
                << '\n';
      return 1;
    }
  }
  if (!Check(!(software && require_hardware), "incompatible command options"))
    return 1;
#if defined(RTC_QSV_H264_SMOKE)
  if (!Check(!diagnostic_selftest || (!require_hardware && !software &&
                                      !diagnostic_directory.empty()),
             "diagnostic selftest needs its directory and no media option"))
    return 1;
  if (!diagnostic_directory.empty() &&
      !Check(InstallQsvCrashDiagnostics(diagnostic_directory),
             "minidump diagnostic setup"))
    return 1;
  if (diagnostic_selftest) {
    std::cerr << "DIAGNOSTIC_SELFTEST QSV_NOT_RUN exception=0xe0425356\n";
    std::cerr.flush();
    RaiseException(0xe0425356UL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 1;
  }
#endif
  if (software) {
    auto encoder = CreateH264Encoder(CreateEnvironment());
    if (!encoder || !RunSequence(*encoder, false))
      return 1;
    std::cout << "PASS SOFTWARE_HARNESS_ONLY " << kHardwareName << "_NOT_RUN\n";
    return 0;
  }
  std::string reason;
  const auto status = ProbeHardwareBackend(&reason);
  if (status == external_encoder::Status::kUnavailable ||
      status == external_encoder::Status::kUnsupported) {
    if (!CheckBuiltinFactory(false))
      return 1;
    std::cout << (require_hardware ? "FAIL" : "SKIP") << ' ' << kHardwareName
              << "_H264 unavailable reason=" << reason << '\n';
    return require_hardware ? 1 : 77;
  }
  if (!Check(status == external_encoder::Status::kOk,
             "requested hardware capability probe")) {
    std::cerr << reason << '\n';
    return 1;
  }
  auto encoder = CreateBackendVideoEncoder(Format(), CreateHardwareBackend());
  if (!encoder || !RunSequence(*encoder, true) || !CheckBuiltinFactory(true))
    return 1;
  std::cout << "PASS " << kHardwareName
            << "_H264_LOCAL_CHAIN network_and_rtcp_pli=NOT_RUN\n";
  return 0;
}

}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
  webrtc::LogMessage::SetLogToStderr(true);
  return webrtc::RunSmoke(argc, argv);
}
