/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

// Local encode/RTP export. The accompanying runner verifies
// these reconstructed access units with an independent HEVC reference decoder.
// This is not a PeerConnection/network/RTCP PLI or product-decoder acceptance.
// RTC_QSV_H265_SMOKE reuses the same gates with the QSV hardware backend.
// RTC_X265_H265_SMOKE requires the real x265 software encoder and factory.
// A replay fixture checks the harness only; it is never a hardware acceptance.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/environment/environment_factory.h"
#if defined(RTC_QSV_H265_SMOKE) || defined(RTC_X265_H265_SMOKE) || \
    defined(RTC_USE_QSV)
#include "api/field_trials.h"
#endif
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h265/h265_common.h"
#include "modules/rtp_rtcp/source/rtp_format.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/rtp_rtcp/source/rtp_video_header.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer_h265.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#if defined(RTC_QSV_H265_SMOKE)
#include "modules/video_coding/codecs/external/qsv_crash_diagnostics.h"  // nogncheck
#include "modules/video_coding/codecs/external/qsv_video_encoder_backend.h"  // nogncheck
#elif defined(RTC_X265_H265_SMOKE)
#include "modules/video_coding/codecs/external/x265_video_encoder_backend.h"  // nogncheck
#else
#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend.h"  // nogncheck
#endif
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;

#if defined(RTC_QSV_H265_SMOKE)
constexpr char kHardwareName[] = "QSV";
constexpr char kSmokeName[] = "qsv_h265_smoke";
constexpr char kRequireHardwareArgument[] = "--require-qsv";
#elif defined(RTC_X265_H265_SMOKE)
constexpr char kHardwareName[] = "x265";
constexpr char kSmokeName[] = "x265_h265_smoke";
constexpr char kRequireHardwareArgument[] = "--require-x265";
#else
constexpr char kHardwareName[] = "NVENC";
constexpr char kSmokeName[] = "nvenc_h265_smoke";
constexpr char kRequireHardwareArgument[] = "--require-nvenc";
#endif

constexpr bool kUsesHardware =
#if defined(RTC_X265_H265_SMOKE)
    false;
#else
    true;
#endif

const char* ImplementationName(bool hardware) {
#if defined(RTC_X265_H265_SMOKE)
  (void)hardware;
  return "x265";
#else
  return hardware ? kHardwareName : "ReplayFixture";
#endif
}

const char* ValidationMode(bool hardware) {
#if defined(RTC_X265_H265_SMOKE)
  (void)hardware;
  return "SOFTWARE_ENCODER_REQUIRED";
#else
  return hardware ? "HARDWARE_REQUIRED" : "SOFTWARE_FIXTURE_ONLY";
#endif
}

bool HasBuiltinFactory(bool hardware) {
#if defined(RTC_X265_H265_SMOKE)
  (void)hardware;
  return true;
#else
  return hardware;
#endif
}

external_encoder::Status ProbeHardwareBackend(std::string* reason) {
#if defined(RTC_QSV_H265_SMOKE)
  return external_encoder::ProbeQsvH265Backend(reason);
#elif defined(RTC_X265_H265_SMOKE)
  return external_encoder::ProbeX265H265Backend(reason);
#else
  return external_encoder::ProbeNvencH265Backend(reason);
#endif
}

std::unique_ptr<external_encoder::VideoEncoderBackend> CreateHardwareBackend() {
#if defined(RTC_QSV_H265_SMOKE)
  return external_encoder::CreateQsvH265Backend();
#elif defined(RTC_X265_H265_SMOKE)
  return external_encoder::CreateX265H265Backend();
#else
  return external_encoder::CreateNvencH265Backend();
#endif
}

Environment HardwareEnvironment() {
#if defined(RTC_X265_H265_SMOKE)
  return CreateEnvironment(
      std::make_unique<FieldTrials>("WebRTC-H265-Encoder/x265/"));
#elif defined(RTC_QSV_H265_SMOKE) || defined(RTC_USE_QSV)
  // Select the requested vendor even when both hardware backends are built.
  return CreateEnvironment(std::make_unique<FieldTrials>(
      std::string("WebRTC-H265-HardwareEncoder/") + kHardwareName + "/"));
#else
  return CreateEnvironment();
#endif
}

bool Check(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL " << message << '\n';
  return condition;
}

SdpVideoFormat Format() {
  return SdpVideoFormat("H265", {{"profile-id", "1"},
                                 {"tier-flag", "0"},
                                 {"level-id", "93"},
                                 {"tx-mode", "SRST"}});
}

VideoCodec CodecSettings() {
  VideoCodec codec;
  codec.codecType = kVideoCodecH265;
  codec.width = kWidth;
  codec.height = kHeight;
  codec.startBitrate = 600;
  codec.minBitrate = 30;
  codec.maxBitrate = 1500;
  codec.maxFramerate = 30;
  codec.active = true;
  codec.numberOfSimulcastStreams = 0;
  codec.SetFrameDropEnabled(false);
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

bool HasNal(const EncodedImage& image, H265::NaluType type) {
  for (const auto& nalu : H265::FindNaluIndices(
           ArrayView<const uint8_t>(image.data(), image.size()))) {
    if (nalu.payload_size >= H265::kNaluHeaderSize &&
        H265::ParseNaluType(image.data()[nalu.payload_start_offset]) == type)
      return true;
  }
  return false;
}

bool IsIdr(const EncodedImage& image) {
  return HasNal(image, H265::kIdrWRadl) || HasNal(image, H265::kIdrNLp);
}

#if defined(RTC_QSV_H265_SMOKE)
// Validation-only input from an independent libx265 fixture. It does not encode
// pixels, implement a product software encoder, or prove vendor rate control.
// The binary format is HEVCFX01, little-endian uint32 AU count, followed by a
// little-endian uint32 length and complete Annex B bytes for each of 60 frames.
class ReplayFixtureBackend final
    : public external_encoder::VideoEncoderBackend {
 public:
  static std::unique_ptr<ReplayFixtureBackend> Load(
      const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    char magic[8];
    input.read(magic, sizeof(magic));
    uint32_t count = 0;
    if (!input || std::string(magic, sizeof(magic)) != "HEVCFX01" ||
        !ReadUint32(input, &count) || count != 60) {
      std::cerr << "FAIL invalid 60-frame HEVC fixture header\n";
      return nullptr;
    }
    auto fixture = std::make_unique<ReplayFixtureBackend>();
    size_t total_bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t bytes = 0;
      if (!ReadUint32(input, &bytes) || bytes == 0 || bytes > 8 * 1024 * 1024 ||
          total_bytes + bytes > 64 * 1024 * 1024) {
        std::cerr << "FAIL invalid HEVC fixture AU length\n";
        return nullptr;
      }
      std::vector<uint8_t> au(bytes);
      input.read(reinterpret_cast<char*>(au.data()), bytes);
      if (!input) {
        std::cerr << "FAIL truncated HEVC fixture AU\n";
        return nullptr;
      }
      fixture->frames_.push_back(std::move(au));
      total_bytes += bytes;
    }
    if (input.peek() != std::char_traits<char>::eof()) {
      std::cerr << "FAIL trailing HEVC fixture bytes\n";
      return nullptr;
    }
    return fixture;
  }

  external_encoder::Info GetInfo() const override {
    return {"ReplayFixture", false};
  }
  external_encoder::Status Open(
      const external_encoder::Config& config) override {
    if (config.codec != external_encoder::Codec::kH265 ||
        config.width != kWidth || config.height != kHeight ||
        config.h265_level != 93 || config.bitrate_bps == 0 ||
        config.framerate_fps == 0) {
      error_ = "fixture expects HEVC Main level93 640x360";
      return external_encoder::Status::kInvalidArgument;
    }
    open_ = true;
    return external_encoder::Status::kOk;
  }
  external_encoder::Status SetRates(
      const external_encoder::Rates& rates) override {
    return open_ && rates.bitrate_bps != 0 && rates.framerate_fps != 0
               ? external_encoder::Status::kOk
               : external_encoder::Status::kInvalidArgument;
  }
  external_encoder::Status Encode(const external_encoder::Frame& frame,
                                  bool force_key_frame,
                                  external_encoder::Packet* packet) override {
    if (!open_ || !packet || frame.width != kWidth || frame.height != kHeight ||
        !frame.y || !frame.uv || frame.stride_y < kWidth ||
        frame.stride_uv < kWidth || cursor_ >= frames_.size()) {
      error_ = "invalid or exhausted fixture input";
      return external_encoder::Status::kInvalidArgument;
    }
    packet->bytes = frames_[cursor_++];
    packet->frame_id = frame.id;
    packet->key_frame = false;
    packet->qp = -1;
    for (const auto& nalu : H265::FindNaluIndices(packet->bytes)) {
      if (nalu.payload_size < H265::kNaluHeaderSize)
        continue;
      const auto type =
          H265::ParseNaluType(packet->bytes[nalu.payload_start_offset]);
      packet->key_frame |= type == H265::kIdrWRadl || type == H265::kIdrNLp;
    }
    if (force_key_frame && !packet->key_frame) {
      error_ = "fixture does not contain the requested IDR";
      return external_encoder::Status::kError;
    }
    return external_encoder::Status::kOk;
  }
  void Close() override { open_ = false; }
  std::string_view LastError() const override { return error_; }

 private:
  static bool ReadUint32(std::istream& input, uint32_t* value) {
    uint8_t bytes[4];
    input.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!input)
      return false;
    *value = static_cast<uint32_t>(bytes[0]) |
             (static_cast<uint32_t>(bytes[1]) << 8) |
             (static_cast<uint32_t>(bytes[2]) << 16) |
             (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
  }
  std::vector<std::vector<uint8_t>> frames_;
  size_t cursor_ = 0;
  bool open_ = false;
  std::string error_;
};
#endif

bool EqualNalPayloads(const EncodedImage& original,
                      const EncodedImage& assembled) {
  const auto lhs = H265::FindNaluIndices(
      ArrayView<const uint8_t>(original.data(), original.size()));
  const auto rhs = H265::FindNaluIndices(
      ArrayView<const uint8_t>(assembled.data(), assembled.size()));
  if (lhs.empty() || lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].payload_size != rhs[i].payload_size ||
        !std::equal(
            original.data() + lhs[i].payload_start_offset,
            original.data() + lhs[i].payload_start_offset + lhs[i].payload_size,
            assembled.data() + rhs[i].payload_start_offset))
      return false;
  }
  return true;
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

class RtpRoundTrip {
 public:
  bool Assemble(const EncodedImage& image,
                const CodecSpecificInfo& info,
                EncodedImage* assembled) {
    if (!Check(info.codecType == kVideoCodecH265, "H265 codec metadata"))
      return false;
    RtpPacketizer::PayloadSizeLimits limits;
    limits.max_payload_len = 1200;
    RTPVideoHeader header;
    header.frame_type = image._frameType;
    auto packetizer = RtpPacketizer::Create(
        RtpPacketizer::PacketizationFormat::kH265,
        ArrayView<const uint8_t>(image.data(), image.size()), limits, header);
    if (!Check(packetizer != nullptr, "H265 packetizer creation"))
      return false;
    const size_t expected_packets = packetizer->NumPackets();
    if (!Check(expected_packets != 0, "H265 RTP packetization"))
      return false;
    std::vector<CopyOnWriteBuffer> payloads;
    size_t markers = 0;
    for (size_t i = 0; i < expected_packets; ++i) {
      RtpPacketToSend packet(nullptr);
      packet.SetTimestamp(image.RtpTimestamp());
      packet.SetSequenceNumber(sequence_++);
      packet.SetSsrc(0x12345678);
      packet.SetPayloadType(101);
      if (!Check(packetizer->NextPacket(&packet), "RTP next packet"))
        return false;
      RtpPacketReceived received;
      if (!Check(received.Parse(packet.data(), packet.size()),
                 "serialized RTP parse") ||
          !Check(received.Timestamp() == image.RtpTimestamp() &&
                     received.SequenceNumber() == packet.SequenceNumber() &&
                     received.Ssrc() == 0x12345678 &&
                     received.PayloadType() == 101,
                 "serialized RTP metadata") ||
          !Check(received.payload_size() >= H265::kNaluHeaderSize &&
                     received.payload_size() <= 1200,
                 "RTP payload size") ||
          !Check(received.Marker() == (i + 1 == expected_packets),
                 "RTP final marker"))
        return false;
      markers += received.Marker() ? 1 : 0;
      const auto type = H265::ParseNaluType(received.payload()[0]);
      fu_packets_ += type == H265::kFu;
      ap_packets_ += type == H265::kAp;
      ++packets_;
      auto parsed = depacketizer_.Parse(CopyOnWriteBuffer(received.payload()));
      if (!Check(parsed.has_value(), "H265 depacketization"))
        return false;
      // H265 Parse already inserts Annex B start codes for single/AP/FU-start.
      payloads.push_back(std::move(parsed->video_payload));
    }
    if (!Check(markers == 1 && packetizer->NumPackets() == 0,
               "RTP frame completion"))
      return false;
    std::vector<ArrayView<const uint8_t>> fragments;
    for (const auto& payload : payloads)
      fragments.emplace_back(payload.data(), payload.size());
    *assembled = image;
    assembled->SetEncodedData(depacketizer_.AssembleFrame(fragments));
    return Check(EqualNalPayloads(image, *assembled),
                 "RTP reconstructed every NAL byte without loss");
  }

  size_t packets() const { return packets_; }
  size_t fu_packets() const { return fu_packets_; }
  size_t ap_packets() const { return ap_packets_; }

 private:
  VideoRtpDepacketizerH265 depacketizer_;
  uint16_t sequence_ = 1;
  size_t packets_ = 0;
  size_t fu_packets_ = 0;
  size_t ap_packets_ = 0;
};

bool WriteI420(std::ostream& out, const VideoFrame& input) {
  const auto buffer = input.video_frame_buffer()->ToI420();
  if (!Check(buffer != nullptr, "reference I420 conversion"))
    return false;
  for (int y = 0; y < kHeight; ++y)
    out.write(
        reinterpret_cast<const char*>(buffer->DataY() + y * buffer->StrideY()),
        kWidth);
  for (int y = 0; y < kHeight / 2; ++y)
    out.write(
        reinterpret_cast<const char*>(buffer->DataU() + y * buffer->StrideU()),
        kWidth / 2);
  for (int y = 0; y < kHeight / 2; ++y)
    out.write(
        reinterpret_cast<const char*>(buffer->DataV() + y * buffer->StrideV()),
        kWidth / 2);
  return Check(out.good(), "write reference I420");
}

class Exporter {
 public:
  explicit Exporter(std::filesystem::path directory,
                    bool hardware = kUsesHardware)
      : directory_(std::move(directory)),
        hardware_(hardware),
        stream_(directory_ / "stream.hevc", std::ios::binary),
        input_(directory_ / "input.i420", std::ios::binary),
        manifest_(directory_ / "manifest.json") {}

  bool Open() {
    if (!Check(stream_.good() && input_.good() && manifest_.good(),
               "open new export files"))
      return false;
    manifest_ << "{\n  \"schemaVersion\": 1,\n"
                 "  \"codec\": \"H265\",\n"
                 "  \"implementation\": \""
              << ImplementationName(hardware_)
              << "\",\n  \"hardware\": " << (hardware_ ? "true" : "false")
              << ",\n  \"validationMode\": \"" << ValidationMode(hardware_)
              << "\",\n"
                 "  \"width\": 640, \"height\": 360,\n"
                 "  \"frameCount\": 60, \"sessionCount\": 2,\n"
                 "  \"streamFile\": \"stream.hevc\",\n"
                 "  \"inputFile\": \"input.i420\",\n"
                 "  \"referenceDecode\": \"NOT_RUN\",\n"
                 "  \"frames\": [\n";
    return manifest_.good();
  }

  bool Add(const EncodedImage& assembled,
           const VideoFrame& input,
           int session,
           int session_index,
           bool required_key) {
    stream_.write(reinterpret_cast<const char*>(assembled.data()),
                  assembled.size());
    if (!Check(stream_.good(), "write reconstructed HEVC") ||
        !WriteI420(input_, input))
      return false;
    const std::string key_name = "required-key-" + std::to_string(frames_);
    if (required_key && !WriteKey(key_name, assembled, input))
      return false;
    if (frames_ != 0)
      manifest_ << ",\n";
    manifest_ << "    {\"index\": " << frames_ << ", \"session\": " << session
              << ", \"sessionIndex\": " << session_index
              << ", \"rtpTimestamp\": " << assembled.RtpTimestamp()
              << ", \"frameId\": " << input.rtp_timestamp()
              << ", \"keyFrame\": " << (IsIdr(assembled) ? "true" : "false")
              << ", \"requiredKey\": " << (required_key ? "true" : "false")
              << ", \"annexBBytes\": " << assembled.size();
    if (required_key) {
      manifest_ << ", \"keyStreamFile\": \"" << key_name
                << ".hevc\", \"keyInputFile\": \"" << key_name << ".i420\"";
      ++required_keys_;
    }
    manifest_ << '}';
    ++frames_;
    bytes_ += assembled.size();
    return Check(manifest_.good(), "write frame metadata");
  }

  bool WriteKey(const std::string& name,
                const EncodedImage& assembled,
                const VideoFrame& input) {
    std::ofstream key_stream(directory_ / (name + ".hevc"), std::ios::binary);
    std::ofstream key_input(directory_ / (name + ".i420"), std::ios::binary);
    key_stream.write(reinterpret_cast<const char*>(assembled.data()),
                     assembled.size());
    if (!Check(key_stream.good(), "write independent reconstructed IDR") ||
        !WriteI420(key_input, input))
      return false;
    key_stream.close();
    key_input.close();
    return Check(!key_stream.fail() && !key_input.fail(), "close key exports");
  }

  bool Finish(const RtpRoundTrip& rtp) {
    if (!Check(frames_ == 60 && required_keys_ == 6,
               "60 frames and six independently decodable key candidates"))
      return false;
    manifest_ << "\n  ],\n  \"requiredKeyCount\": " << required_keys_
              << ",\n  \"encodedBytes\": " << bytes_
              << ",\n  \"rtpPackets\": " << rtp.packets()
              << ",\n  \"fuPackets\": " << rtp.fu_packets()
              << ",\n  \"apPackets\": " << rtp.ap_packets()
              << ",\n  \"builtinFactory\": {\"hardware\": "
              << (hardware_ ? "true" : "false") << ", \"implementation\": \""
              << (HasBuiltinFactory(hardware_) ? kHardwareName : "NOT_RUN")
              << "\", \"streamFile\": \""
              << (HasBuiltinFactory(hardware_) ? "builtin-factory.hevc" : "")
              << "\", \"inputFile\": \""
              << (HasBuiltinFactory(hardware_) ? "builtin-factory.i420" : "")
              << "\"}\n}\n";
    stream_.close();
    input_.close();
    manifest_.close();
    return Check(!stream_.fail() && !input_.fail() && !manifest_.fail(),
                 "close exported evidence");
  }

 private:
  std::filesystem::path directory_;
  const bool hardware_;
  std::ofstream stream_;
  std::ofstream input_;
  std::ofstream manifest_;
  size_t frames_ = 0;
  size_t required_keys_ = 0;
  size_t bytes_ = 0;
};

void SetRates(VideoEncoder& encoder, uint32_t bps, double fps) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, bps);
  encoder.SetRates(VideoEncoder::RateControlParameters(allocation, fps));
}

bool EncodeAndAssemble(VideoEncoder& encoder,
                       EncodeCollector& collector,
                       RtpRoundTrip& rtp,
                       const VideoFrame& input,
                       bool force_key,
                       bool require_key,
                       EncodedImage* assembled,
                       bool hardware = kUsesHardware) {
  collector.Clear();
  const std::vector<VideoFrameType> key = {VideoFrameType::kVideoFrameKey};
  if (!Check(encoder.Encode(input, force_key ? &key : nullptr) ==
                 WEBRTC_VIDEO_CODEC_OK,
             "encode return status") ||
      !Check(collector.images.size() == 1 && collector.infos.size() == 1,
             "synchronous complete access unit"))
    return false;
  const auto& image = collector.images[0];
  if (!Check(image.RtpTimestamp() == input.rtp_timestamp(),
             "encoded frame ID / RTP timestamp") ||
      !Check(image._encodedWidth == kWidth && image._encodedHeight == kHeight,
             "encoded dimensions") ||
      !Check(
          (image._frameType == VideoFrameType::kVideoFrameKey) == IsIdr(image),
          "real IDR (NAL 19/20) matches callback metadata"))
    return false;
  if (require_key &&
      !Check(IsIdr(image) && HasNal(image, H265::kVps) &&
                 HasNal(image, H265::kSps) && HasNal(image, H265::kPps),
             "requested/resumed IDR with in-band VPS/SPS/PPS"))
    return false;
  if (!rtp.Assemble(image, collector.infos[0], assembled))
    return false;
  return Check(encoder.GetEncoderInfo().is_hardware_accelerated == hardware &&
                   encoder.GetEncoderInfo().implementation_name.find(
                       ImplementationName(hardware)) != std::string::npos,
               "encoder remained on the requested implementation");
}

bool RunSequence(VideoEncoder& encoder,
                 Exporter& exporter,
                 RtpRoundTrip& rtp,
                 bool hardware = kUsesHardware) {
  EncodeCollector collector;
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
    const auto identity = encoder.GetEncoderInfo();
    if (!Check(identity.is_hardware_accelerated == hardware &&
                   identity.implementation_name.find(
                       ImplementationName(hardware)) != std::string::npos,
               "requested implementation hardware identity"))
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
              !Check(collector.images.empty() && collector.infos.empty(),
                     "paused output suppression"))
            return false;
        }
        SetRates(encoder, 350000, 15);
      }
      timestamp += i >= 15 ? 6000 : 3000;
      const auto frame = MakeFrame(session * 30 + i, timestamp);
      const bool require_key = i == 0 || i == 10 || i == 20;
      EncodedImage assembled;
      if (!EncodeAndAssemble(encoder, collector, rtp, frame, i == 10,
                             require_key, &assembled, hardware) ||
          !exporter.Add(assembled, frame, session, i, require_key))
        return false;
    }
    if (!Check(encoder.Release() == WEBRTC_VIDEO_CODEC_OK, "encoder release") ||
        !Check(encoder.Release() == WEBRTC_VIDEO_CODEC_OK,
               "idempotent encoder release"))
      return false;
  }
  if (!Check(rtp.fu_packets() != 0, "H265 RTP fragmentation exercised"))
    return false;
  std::cout << "PASS " << ImplementationName(hardware)
            << "_H265_RTP_EXPORT frames=60 fu_packets=" << rtp.fu_packets()
            << " ap_packets=" << rtp.ap_packets()
            << " reference_decode=NOT_RUN\n"
               "PASS key_request rates_change pause_resume restart\n";
  return true;
}

bool CheckBuiltinFactory(Exporter& exporter) {
  auto factory = CreateBuiltinVideoEncoderFactory();
  const auto formats = factory->GetSupportedFormats();
  if (!Check(
          std::any_of(formats.begin(), formats.end(),
                      [](const SdpVideoFormat& f) { return f.name == "H265"; }),
          "builtin factory advertises requested H265 implementation"))
    return false;
  auto encoder = factory->Create(HardwareEnvironment(), Format());
  if (!Check(encoder != nullptr, "builtin H265 factory creation"))
    return false;
  EncodeCollector collector;
  RtpRoundTrip rtp;
  const auto codec = CodecSettings();
  if (!Check(encoder->RegisterEncodeCompleteCallback(&collector) ==
                 WEBRTC_VIDEO_CODEC_OK,
             "builtin callback registration") ||
      !Check(encoder->InitEncode(&codec, VideoEncoder::Settings(
                                             VideoEncoder::Capabilities(false),
                                             1, 1200)) == WEBRTC_VIDEO_CODEC_OK,
             "builtin encoder initialization"))
    return false;
  const auto input = MakeFrame(1, 90000);
  EncodedImage assembled;
  if (!EncodeAndAssemble(*encoder, collector, rtp, input, true, true,
                         &assembled) ||
      !exporter.WriteKey("builtin-factory", assembled, input))
    return false;
  const auto identity = encoder->GetEncoderInfo();
  if (!Check(encoder->Release() == WEBRTC_VIDEO_CODEC_OK,
             "builtin encoder release"))
    return false;
  std::cout << "PASS builtin_factory implementation="
            << identity.implementation_name
            << " hardware=" << (kUsesHardware ? 1 : 0) << '\n';
  return true;
}

int RunSmoke(int argc, char** argv) {
  std::string output_directory;
#if defined(RTC_QSV_H265_SMOKE)
  bool require_hardware = false;
  std::string diagnostic_directory;
  bool diagnostic_selftest = false;
  std::string fixture_path;
#else
  bool require_hardware = true;
#endif
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument == "--output-dir" && i + 1 < argc) {
      output_directory = argv[++i];
    } else if (argument == kRequireHardwareArgument) {
      require_hardware = true;
#if defined(RTC_QSV_H265_SMOKE)
    } else if (argument == "--diagnostic-dir" && i + 1 < argc) {
      diagnostic_directory = argv[++i];
      if (!Check(!diagnostic_directory.empty(), "empty diagnostic directory"))
        return 1;
    } else if (argument == "--diagnostic-selftest") {
      diagnostic_selftest = true;
    } else if (argument == "--software-fixture" && i + 1 < argc) {
      fixture_path = argv[++i];
      if (!Check(!fixture_path.empty(), "empty fixture file"))
        return 1;
#endif
    } else {
      std::cerr << "Usage: " << kSmokeName
                << " --output-dir <new existing directory> ["
                << kRequireHardwareArgument << "]"
#if defined(RTC_QSV_H265_SMOKE)
                << " [--diagnostic-dir <existing local directory>]"
                   " [--software-fixture <validation-only HEVCFX01 file>]\n"
                   "       "
                << kSmokeName
                << " --diagnostic-dir <existing local directory>"
                   " --diagnostic-selftest (diagnostics only)"
#endif
                << '\n';
      return 1;
    }
  }
#if defined(RTC_QSV_H265_SMOKE)
  if (!Check(!diagnostic_selftest ||
                 (!require_hardware && fixture_path.empty() &&
                  output_directory.empty() && !diagnostic_directory.empty()),
             "diagnostic selftest needs its directory and no media option") ||
      !Check(!(require_hardware && !fixture_path.empty()),
             "software fixture cannot satisfy hardware requirements"))
    return 1;
  if (!diagnostic_directory.empty() &&
      !Check(qsv_smoke_diagnostics::Install(diagnostic_directory,
                                            L"qsv_h265_smoke"),
             "minidump diagnostic setup"))
    return 1;
  if (diagnostic_selftest) {
    std::cerr << "DIAGNOSTIC_SELFTEST QSV_NOT_RUN exception=0xe0425356\n";
    std::cerr.flush();
    RaiseException(0xe0425356UL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 1;
  }
#endif
  if (!Check(!output_directory.empty(), "missing output directory"))
    return 1;
  const std::filesystem::path directory(output_directory);
  std::error_code error;
  if (!Check(std::filesystem::is_directory(directory, error) && !error,
             "output directory must already exist"))
    return 1;
  if (!Check(std::filesystem::is_empty(directory, error) && !error,
             "output directory must be empty; preserve prior evidence"))
    return 1;
#if defined(RTC_QSV_H265_SMOKE)
  if (!fixture_path.empty()) {
    auto backend = ReplayFixtureBackend::Load(fixture_path);
    if (!backend)
      return 1;
    Exporter exporter(directory, false);
    RtpRoundTrip rtp;
    auto encoder = CreateBackendVideoEncoder(Format(), std::move(backend));
    if (!exporter.Open() ||
        !Check(encoder != nullptr, "fixture adapter creation") ||
        !RunSequence(*encoder, exporter, rtp, false) || !exporter.Finish(rtp))
      return 1;
    std::cout << "PASS SOFTWARE_HARNESS_ONLY QSV_NOT_RUN"
                 " reference_decode=NOT_RUN builtin_factory=NOT_RUN"
                 " vendor_rates=NOT_RUN network_and_rtcp_pli=NOT_RUN\n";
    return 0;
  }
#endif
  std::string reason;
  const auto status = ProbeHardwareBackend(&reason);
  if (status == external_encoder::Status::kUnavailable ||
      status == external_encoder::Status::kUnsupported) {
#if defined(RTC_QSV_H265_SMOKE)
    // An explicit unavailable QSV selection must not quietly initialize
    // another vendor or the x265 software backend.
    auto factory = CreateBuiltinVideoEncoderFactory();
    auto encoder = factory->Create(HardwareEnvironment(), Format());
    if (encoder) {
      const auto codec = CodecSettings();
      if (!Check(encoder->InitEncode(
                     &codec, VideoEncoder::Settings(
                                 VideoEncoder::Capabilities(false), 1, 1200)) !=
                     WEBRTC_VIDEO_CODEC_OK,
                 "unavailable QSV does not initialize a different H265 vendor"))
        return 1;
      encoder->Release();
    }
    std::cout << "PASS builtin_factory unavailable QSV_H265 fallback=NONE\n";
#endif
    std::cout << (require_hardware ? "FAIL" : "SKIP") << ' ' << kHardwareName
              << "_H265_NOT_RUN reason=" << reason << '\n';
    return require_hardware ? 1 : 77;
  }
  if (!Check(status == external_encoder::Status::kOk,
             "required H265 implementation capability probe")) {
    std::cerr << kHardwareName << "_H265_NOT_RUN reason=" << reason << '\n';
    return 1;
  }
  Exporter exporter(directory);
  RtpRoundTrip rtp;
  auto encoder = CreateBackendVideoEncoder(Format(), CreateHardwareBackend());
  if (!exporter.Open() || !Check(encoder != nullptr, "H265 adapter creation") ||
      !RunSequence(*encoder, exporter, rtp) || !CheckBuiltinFactory(exporter) ||
      !exporter.Finish(rtp))
    return 1;
  std::cout << "PASS " << kHardwareName
            << "_H265_LOCAL_RTP_EXPORT reference_decode=NOT_RUN"
               " network_and_rtcp_pli=NOT_RUN\n";
  return 0;
}

}  // namespace
}  // namespace webrtc

int main(int argc, char** argv) {
  webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
  webrtc::LogMessage::SetLogToStderr(true);
  return webrtc::RunSmoke(argc, argv);
}
