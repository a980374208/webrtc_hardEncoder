/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#include "modules/video_coding/codecs/external/backend_video_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "api/array_view.h"
#include "api/sequence_checker.h"
#include "api/video/encoded_image.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/h264_profile_level_id.h"
#if defined(RTC_ENABLE_H265)
#include "api/video_codecs/h265_profile_tier_level.h"
#include "common_video/h265/h265_bitstream_parser.h"
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_sps_parser.h"
#include "rtc_base/bitstream_reader.h"
#endif
#include "api/video_codecs/scalability_mode.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/h264/h264_common.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {

using external_encoder::Status;

uint8_t NegotiatedLevel(const SdpVideoFormat& format) {
  if (PayloadStringToCodecType(format.name) == kVideoCodecH264) {
    return static_cast<uint8_t>(
        ParseSdpForH264ProfileLevelId(format.parameters)->level);
  }
#if defined(RTC_ENABLE_H265)
  return static_cast<uint8_t>(
      ParseSdpForH265ProfileTierLevel(format.parameters)->level);
#else
  return 0;
#endif
}

class BackendVideoEncoder final : public VideoEncoder {
 public:
  BackendVideoEncoder(
      const SdpVideoFormat& format,
      std::unique_ptr<external_encoder::VideoEncoderBackend> backend,
      BackendFailurePolicy failure_policy)
      : backend_(std::move(backend)),
        info_(backend_->GetInfo()),
        codec_type_(PayloadStringToCodecType(format.name)),
        level_(NegotiatedLevel(format)),
        failure_policy_(failure_policy) {
    sequence_checker_.Detach();
  }

  ~BackendVideoEncoder() override { backend_->Close(); }

  int InitEncode(const VideoCodec* codec, const Settings& settings) override {
    RTC_DCHECK_RUN_ON(&sequence_checker_);
    backend_->Close();
    initialized_ = false;
    failed_ = false;
    if (retirement_failed_ || backend_->LastCloseStatus() != Status::kOk) {
      retirement_failed_ = true;
      failed_ = true;
      RTC_LOG(LS_WARNING) << info_.implementation_name
                          << ": InitEncode cleanup: " << backend_->LastError();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (!codec || codec->codecType != codec_type_ || codec->width == 0 ||
        codec->height == 0 || codec->width % 2 || codec->height % 2 ||
        codec->maxFramerate == 0 || settings.number_of_cores <= 0) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (codec->numberOfSimulcastStreams > 1 ||
        (codec_type_ == kVideoCodecH264 &&
         codec->H264().numberOfTemporalLayers > 1) ||
        codec->simulcastStream[0].numberOfTemporalLayers > 1 ||
        (codec->GetScalabilityMode() &&
         codec->GetScalabilityMode() != ScalabilityMode::kL1T1)) {
      return FailureStatus();
    }
    if (codec->startBitrate > std::numeric_limits<uint32_t>::max() / 1000) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    codec_ = *codec;
    paused_ = codec->startBitrate == 0;
    need_key_frame_ = true;
    external_encoder::Config config;
    config.codec = codec_type_ == kVideoCodecH264
                       ? external_encoder::Codec::kH264
                       : external_encoder::Codec::kH265;
    config.width = codec->width;
    config.height = codec->height;
    // A paused HEVC level-1 session must also stay within its bitrate limit.
    const uint32_t initial_bitrate =
        codec_type_ == kVideoCodecH265 ? 64000 : 300000;
    config.bitrate_bps = paused_ ? initial_bitrate : codec->startBitrate * 1000;
    config.framerate_fps = codec->maxFramerate;
    if (codec_type_ == kVideoCodecH264) {
      config.h264_level = level_;
    } else {
      config.h265_level = level_;
#if defined(RTC_ENABLE_H265)
      h265_bitstream_parser_ = std::make_unique<H265BitstreamParser>();
#endif
    }
    const Status open_status = backend_->Open(config);
    if (open_status != Status::kOk) {
      return FailBackend(open_status);
    }
    initialized_ = true;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override {
    RTC_DCHECK_RUN_ON(&sequence_checker_);
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    RTC_DCHECK_RUN_ON(&sequence_checker_);
    initialized_ = false;
    failed_ = false;
    backend_->Close();
    retirement_failed_ |= backend_->LastCloseStatus() != Status::kOk;
    const bool close_ok = !retirement_failed_;
    if (!close_ok) {
      RTC_LOG(LS_WARNING) << info_.implementation_name
                          << ": Release: " << backend_->LastError();
    }
    sequence_checker_.Detach();
    return close_ok ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
  }

  void SetRates(const RateControlParameters& parameters) override {
    RTC_DCHECK_RUN_ON(&sequence_checker_);
    if (!initialized_ || failed_) {
      return;
    }
    const uint32_t bitrate = parameters.bitrate.get_sum_bps();
    if (bitrate == 0) {
      paused_ = true;
      need_key_frame_ = true;
      return;
    }
    if (!std::isfinite(parameters.framerate_fps) ||
        parameters.framerate_fps > std::numeric_limits<uint32_t>::max()) {
      failed_ = true;
      return;
    }
    const uint32_t framerate =
        parameters.framerate_fps <= 0
            ? codec_.maxFramerate
            : static_cast<uint32_t>(std::max(1.0, parameters.framerate_fps));
    const external_encoder::Rates rates{bitrate, framerate};
    const Status rate_status = backend_->SetRates(rates);
    if (rate_status != Status::kOk) {
      FailBackend(rate_status);
      return;
    }
    need_key_frame_ |= paused_;
    paused_ = false;
  }

  int32_t Encode(const VideoFrame& input,
                 const std::vector<VideoFrameType>* frame_types) override {
    RTC_DCHECK_RUN_ON(&sequence_checker_);
    if (failed_) {
      return FailureStatus();
    }
    if (!initialized_ || !callback_) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    if (paused_ || (frame_types && !frame_types->empty() &&
                    frame_types->front() == VideoFrameType::kEmptyFrame)) {
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if (input.width() != codec_.width || input.height() != codec_.height) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    const auto source = input.video_frame_buffer();
    if (!source) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    scoped_refptr<const NV12BufferInterface> nv12;
    if (source->type() == VideoFrameBuffer::Type::kNV12) {
      nv12 = source->GetNV12();
    } else {
      auto i420 = source->ToI420();
      if (!i420) {
        return FailureStatus();
      }
      nv12 = NV12Buffer::Copy(*i420);
    }
    const bool request_key_frame =
        need_key_frame_ ||
        (frame_types && !frame_types->empty() &&
         frame_types->front() == VideoFrameType::kVideoFrameKey);
    external_encoder::Frame frame;
    frame.y = nv12->DataY();
    frame.uv = nv12->DataUV();
    frame.stride_y = nv12->StrideY();
    frame.stride_uv = nv12->StrideUV();
    frame.width = input.width();
    frame.height = input.height();
    frame.id = input.rtp_timestamp();
    external_encoder::Packet packet;
    const Status status = backend_->Encode(frame, request_key_frame, &packet);
    if (status == Status::kNoOutput) {
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if (status != Status::kOk || packet.bytes.empty() ||
        packet.frame_id != frame.id) {
      return FailBackend(status);
    }

    bool idr = false;
    if (codec_type_ == kVideoCodecH265) {
#if defined(RTC_ENABLE_H265)
      if (!ValidateH265Packet(packet, request_key_frame, &idr)) {
        return FailBackend();
      }
#endif
    } else {
      bool sps = false;
      bool pps = false;
      const auto nalus = H264::FindNaluIndices(packet.bytes);
      for (const auto& nalu : nalus) {
        if (nalu.payload_size == 0) {
          return FailBackend();
        }
        const auto type =
            H264::ParseNaluType(packet.bytes[nalu.payload_start_offset]);
        idr |= type == H264::kIdr;
        pps |= type == H264::kPps;
        if (type == H264::kSps) {
          // Baseline and its constrained subset are both decodable after
          // Baseline negotiation. Never advertise a level below the actual SPS.
          if (nalu.payload_size < 4 ||
              packet.bytes[nalu.payload_start_offset + 1] != 66 ||
              packet.bytes[nalu.payload_start_offset + 3] >
                  static_cast<uint8_t>(level_)) {
            return FailBackend();
          }
          sps = true;
        }
      }
      if (nalus.empty() || packet.key_frame != idr ||
          (request_key_frame && (!idr || !sps || !pps))) {
        return FailBackend();
      }
    }
    need_key_frame_ = false;

    EncodedImage encoded;
    encoded.SetEncodedData(
        EncodedImageBuffer::Create(packet.bytes.data(), packet.bytes.size()));
    encoded._encodedWidth = codec_.width;
    encoded._encodedHeight = codec_.height;
    encoded.SetRtpTimestamp(input.rtp_timestamp());
    encoded.ntp_time_ms_ = input.ntp_time_ms();
    encoded.capture_time_ms_ = input.render_time_ms();
    encoded.rotation_ = input.rotation();
    encoded.SetColorSpace(input.color_space());
    encoded._frameType =
        idr ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;
    encoded.content_type_ = codec_.mode == VideoCodecMode::kScreensharing
                                ? VideoContentType::SCREENSHARE
                                : VideoContentType::UNSPECIFIED;
    if (codec_type_ == kVideoCodecH264) {
      bitstream_parser_.ParseBitstream(
          ArrayView<const uint8_t>(encoded.data(), encoded.size()));
      encoded.qp_ = bitstream_parser_.GetLastSliceQp().value_or(packet.qp);
    } else {
#if defined(RTC_ENABLE_H265)
      h265_bitstream_parser_->ParseBitstream(
          ArrayView<const uint8_t>(encoded.data(), encoded.size()));
      encoded.qp_ =
          h265_bitstream_parser_->GetLastSliceQp().value_or(packet.qp);
#endif
    }

    CodecSpecificInfo codec_specific;
    codec_specific.codecType = codec_type_;
    if (codec_type_ == kVideoCodecH264) {
      codec_specific.codecSpecific.H264.packetization_mode =
          H264PacketizationMode::NonInterleaved;
      codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
      codec_specific.codecSpecific.H264.idr_frame = idr;
      codec_specific.codecSpecific.H264.base_layer_sync = false;
    }
    // A callback may synchronously Release this encoder; do not touch members
    // after invoking it. EncodedImage owns the output independently of backend.
    return callback_->OnEncodedImage(encoded, &codec_specific).error ==
                   EncodedImageCallback::Result::OK
               ? WEBRTC_VIDEO_CODEC_OK
               : WEBRTC_VIDEO_CODEC_ERROR;
  }

  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = info_.implementation_name;
    info.is_hardware_accelerated = info_.is_hardware_accelerated;
    info.supports_native_handle = false;
    info.supports_simulcast = false;
    info.requested_resolution_alignment = 2;
    info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNV12,
                                    VideoFrameBuffer::Type::kI420};
    info.has_trusted_rate_controller = false;
    return info;
  }

 private:
  int FailureStatus() const {
    return codec_type_ == kVideoCodecH265 &&
                   failure_policy_ == BackendFailurePolicy::kDefault
               ? WEBRTC_VIDEO_CODEC_ENCODER_FAILURE
               : WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

#if defined(RTC_ENABLE_H265)
  bool ValidateH265Packet(const external_encoder::Packet& packet,
                          bool request_key_frame,
                          bool* idr) const {
    bool vps = false, sps = false, pps = false, vcl = false;
    const auto nalus = H265::FindNaluIndices(packet.bytes);
    for (const auto& nalu : nalus) {
      const auto payload =
          ArrayView<const uint8_t>(packet.bytes)
              .subview(nalu.payload_start_offset, nalu.payload_size);
      // SRST, layer 0, one temporal layer; reject forbidden or reserved
      // headers.
      if (payload.size() < 2 || (payload[0] & 0x81) != 0 || payload[1] != 1) {
        return false;
      }
      const auto type = H265::ParseNaluType(payload[0]);
      // Reserved VCL/IRAP types and RTP payload wrappers are not Annex B NALs
      // supported by this encoder. A picture slice needs data after its header.
      const bool picture_slice = type <= H265::kRaslR ||
                                 type == H265::kIdrWRadl ||
                                 type == H265::kIdrNLp;
      if ((!picture_slice && (type < H265::kVps || type > H265::kSuffixSei)) ||
          (picture_slice && payload.size() <= 2)) {
        return false;
      }
      *idr |= type == H265::kIdrWRadl || type == H265::kIdrNLp;
      vcl |= picture_slice;
      vps |= type == H265::kVps;
      pps |= type == H265::kPps;
      if (type == H265::kSps) {
        const auto data = payload.subview(2);
        const auto parsed = H265SpsParser::ParseSps(data);
        const auto rbsp = H265::ParseRbsp(data);
        BitstreamReader reader(rbsp);
        reader.ConsumeBits(4);  // sps_video_parameter_set_id.
        const uint32_t max_sub_layers = reader.ReadBits(3);
        reader.ConsumeBits(1);  // sps_temporal_id_nesting_flag.
        if (!reader.Ok() || max_sub_layers != 0) {
          return false;
        }
        // The common PTL parser consumes but does not expose space/tier.
        BitstreamReader tier_reader(rbsp);
        tier_reader.ConsumeBits(8);
        const uint32_t profile_space = tier_reader.ReadBits(2);
        const bool high_tier = tier_reader.Read<bool>();
        const auto ptl = H265SpsParser::ParseProfileTierLevel(true, 0, reader);
        if (!tier_reader.Ok() || profile_space != 0 || high_tier || !ptl ||
            ptl->general_profile_idc != 1 ||
            !StringToH265Level(std::to_string(ptl->general_level_idc)) ||
            ptl->general_level_idc > level_ || !parsed ||
            parsed->sps_max_sub_layers_minus1 != 0 ||
            parsed->bit_depth_luma_minus8 != 0 ||
            parsed->chroma_format_idc != 1 || parsed->width != codec_.width ||
            parsed->height != codec_.height) {
          return false;
        }
        // SpsState exposes only the luma bit depth. Read the same prefix to
        // enforce 8-bit chroma too, without changing the shared parser API.
        reader.ReadExponentialGolomb();  // sps_seq_parameter_set_id.
        reader.ReadExponentialGolomb();  // chroma_format_idc, checked above.
        reader.ReadExponentialGolomb();  // pic_width_in_luma_samples.
        reader.ReadExponentialGolomb();  // pic_height_in_luma_samples.
        if (reader.Read<bool>()) {       // conformance_window_flag.
          for (int i = 0; i < 4; ++i) {
            reader.ReadExponentialGolomb();
          }
        }
        const uint32_t luma_depth = reader.ReadExponentialGolomb();
        const uint32_t chroma_depth = reader.ReadExponentialGolomb();
        if (!reader.Ok() || luma_depth != 0 || chroma_depth != 0) {
          return false;
        }
        sps = true;
      }
    }
    return !nalus.empty() && vcl && packet.key_frame == *idr &&
           (!request_key_frame || (*idr && vps && sps && pps));
  }
#endif

  int FailBackend(Status status = Status::kError) {
    failed_ = true;
    retirement_failed_ |= status == Status::kRetirementFailed;
    RTC_LOG(LS_WARNING) << info_.implementation_name << ": "
                        << backend_->LastError();
    return FailureStatus();
  }

  SequenceChecker sequence_checker_;
  std::unique_ptr<external_encoder::VideoEncoderBackend> backend_;
  const external_encoder::Info info_;
  const VideoCodecType codec_type_;
  const uint8_t level_;
  const BackendFailurePolicy failure_policy_;
  VideoCodec codec_;
  EncodedImageCallback* callback_ = nullptr;
  H264BitstreamParser bitstream_parser_;
#if defined(RTC_ENABLE_H265)
  std::unique_ptr<H265BitstreamParser> h265_bitstream_parser_;
#endif
  bool initialized_ = false;
  bool failed_ = false;
  // A process/vendor retirement failure can block Open without this new
  // instance owning a session. Release must still prevent software success.
  bool retirement_failed_ = false;
  bool paused_ = false;
  bool need_key_frame_ = true;
};

}  // namespace

bool IsBackendH264FormatSupported(const SdpVideoFormat& format) {
  if (PayloadStringToCodecType(format.name) != kVideoCodecH264) {
    return false;
  }
  const auto mode = format.parameters.find("packetization-mode");
  const auto profile = ParseSdpForH264ProfileLevelId(format.parameters);
  return mode != format.parameters.end() && mode->second == "1" && profile &&
         profile->profile == H264Profile::kProfileBaseline &&
         profile->level != H264Level::kLevel1_b;
}

bool IsNvencH264FormatSupported(const SdpVideoFormat& format) {
  return IsBackendH264FormatSupported(format);
}

bool IsBackendH265FormatSupported(const SdpVideoFormat& format) {
#if defined(RTC_ENABLE_H265)
  if (PayloadStringToCodecType(format.name) != kVideoCodecH265) {
    return false;
  }
  const auto ptl = ParseSdpForH265ProfileTierLevel(format.parameters);
  const auto tx_mode = format.parameters.find("tx-mode");
  const auto don_diff = format.parameters.find("sprop-max-don-diff");
  return ptl && ptl->profile == H265Profile::kProfileMain &&
         ptl->tier == H265Tier::kTier0 &&
         (tx_mode == format.parameters.end() ||
          (tx_mode->second.size() == 4 &&
           std::equal(tx_mode->second.begin(), tx_mode->second.end(), "SRST",
                      [](char left, char right) {
                        return left == right || left == right + ('a' - 'A');
                      }))) &&
         (don_diff == format.parameters.end() || don_diff->second == "0");
#else
  return false;
#endif
}

bool IsNvencH265FormatSupported(const SdpVideoFormat& format) {
  return IsBackendH265FormatSupported(format);
}

std::unique_ptr<VideoEncoder> CreateBackendVideoEncoder(
    const SdpVideoFormat& format,
    std::unique_ptr<external_encoder::VideoEncoderBackend> backend,
    BackendFailurePolicy failure_policy) {
  if (!backend || (!IsBackendH264FormatSupported(format) &&
                   !IsBackendH265FormatSupported(format))) {
    return nullptr;
  }
  return std::make_unique<BackendVideoEncoder>(format, std::move(backend),
                                               failure_policy);
}

}  // namespace webrtc
