/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "media/engine/internal_encoder_factory.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"  // nogncheck
#endif
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#if defined(WEBRTC_USE_H264)
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"  // nogncheck
#endif
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_X265) || \
    defined(RTC_USE_AMF)
#include "modules/video_coding/codecs/external/backend_video_encoder.h"  // nogncheck
#include "modules/video_coding/codecs/external/h265_encoder_policy.h"  // nogncheck
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"  // nogncheck
#include "modules/video_coding/codecs/external/hardware_h264_encoder_policy.h"  // nogncheck
#endif
#if defined(RTC_USE_AMF)
#include "modules/video_coding/codecs/external/amf_video_encoder.h"  // nogncheck
#endif
#if defined(RTC_USE_NVENC)
#include "modules/video_coding/codecs/external/nvenc_video_encoder.h"  // nogncheck
#endif
#if defined(RTC_USE_QSV)
#include "modules/video_coding/codecs/external/qsv_video_encoder.h"  // nogncheck
#endif
#if defined(RTC_USE_X265)
#include "modules/video_coding/codecs/external/x265_video_encoder.h"  // nogncheck
#endif
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
#include "rtc_base/logging.h"
#endif

namespace webrtc {
namespace {

using Factory = VideoEncoderFactoryTemplate<LibvpxVp8EncoderTemplateAdapter,
#if defined(WEBRTC_USE_H264)
                                            OpenH264EncoderTemplateAdapter,
#endif
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
                                            LibaomAv1EncoderTemplateAdapter,
#endif
                                            LibvpxVp9EncoderTemplateAdapter>;

#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
using external_encoder::Status;

bool HardwareRetirementFailed() {
  std::string reason;
  if (external_encoder::HardwareEncoderProbes().CheckAny(&reason) !=
      Status::kRetirementFailed) {
    return false;
  }
  RTC_LOG(LS_ERROR) << "Encoder factory action=blocked "
                       "reason=hardware_retirement_failure "
                    << reason;
  return true;
}

bool IsHardwareCodec(const SdpVideoFormat& format) {
  const auto codec = PayloadStringToCodecType(format.name);
  return codec == kVideoCodecH264 || codec == kVideoCodecH265;
}

Status ProbeHardwareFormat(const SdpVideoFormat& format) {
  if (HardwareRetirementFailed()) {
    return Status::kRetirementFailed;
  }
  Status status = Status::kUnavailable;
#if defined(RTC_USE_NVENC)
  status = ProbeNvencVideoEncoderAvailability(format);
  if (status == Status::kRetirementFailed || HardwareRetirementFailed())
    return Status::kRetirementFailed;
  if (status == Status::kOk)
    return status;
#endif
#if defined(RTC_USE_QSV)
  status = ProbeQsvVideoEncoderAvailability(format);
  if (status == Status::kRetirementFailed || HardwareRetirementFailed())
    return Status::kRetirementFailed;
  if (status == Status::kOk)
    return status;
#endif
#if defined(RTC_USE_AMF)
  status = ProbeAmfVideoEncoderAvailability(format);
  if (status == Status::kRetirementFailed || HardwareRetirementFailed())
    return Status::kRetirementFailed;
#endif
  return status;
}

void RemoveHardwareCodecFormats(std::vector<SdpVideoFormat>& formats) {
  // Discovery has no explicit software-selection environment. Keep unrelated
  // codecs, but do not advertise H264/H265 software success after a fatal
  // hardware probe. Explicit x265 creation remains a separate user choice.
  formats.erase(std::remove_if(formats.begin(), formats.end(),
                               [](const SdpVideoFormat& format) {
                                 return IsHardwareCodec(format);
                               }),
                formats.end());
}

template <typename Creator>
std::unique_ptr<VideoEncoder> CreateHardwareWithRetirementCheck(
    Creator create,
    bool& retirement_failed) {
  if (retirement_failed || HardwareRetirementFailed()) {
    retirement_failed = true;
    return nullptr;
  }
  Status probe_status = Status::kUnavailable;
  auto encoder = create(&probe_status);
  retirement_failed =
      probe_status == Status::kRetirementFailed || HardwareRetirementFailed();
  return retirement_failed ? nullptr : std::move(encoder);
}
#endif
}  // namespace

std::vector<SdpVideoFormat> InternalEncoderFactory::GetSupportedFormats()
    const {
  auto formats = Factory().GetSupportedFormats();
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
  if (HardwareRetirementFailed()) {
    RemoveHardwareCodecFormats(formats);
    return formats;
  }
#endif
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF)
  const SdpVideoFormat h264_1080("H264", {{"level-asymmetry-allowed", "1"},
                                          {"packetization-mode", "1"},
                                          {"profile-level-id", "420028"}});
  const Status h264_status = ProbeHardwareFormat(h264_1080);
  if (h264_status == Status::kRetirementFailed) {
    RemoveHardwareCodecFormats(formats);
    return formats;
  }
  if (h264_status == Status::kOk) {
    for (auto& format : formats) {
      if (IsBackendH264FormatSupported(format)) {
        format.parameters["profile-level-id"] = "420028";
      }
    }
  }
#endif
#if (defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || \
     defined(RTC_USE_X265) || defined(RTC_USE_AMF)) && \
    defined(RTC_ENABLE_H265)
  for (const char* level : {"120", "93"}) {
    SdpVideoFormat h265("H265", {{"profile-id", "1"},
                                 {"tier-flag", "0"},
                                 {"level-id", level},
                                 {"tx-mode", "SRST"}});
    const Status hardware_status = ProbeHardwareFormat(h265);
    if (hardware_status == Status::kRetirementFailed) {
      RemoveHardwareCodecFormats(formats);
      return formats;
    }
    bool available = hardware_status == Status::kOk;
#if defined(RTC_USE_X265)
    available = available || IsX265VideoEncoderAvailable(h265);
#endif
    if (HardwareRetirementFailed()) {
      RemoveHardwareCodecFormats(formats);
      return formats;
    }
    if (available) {
      formats.push_back(std::move(h265));
      break;
    }
  }
#endif
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
  if (HardwareRetirementFailed()) {
    RemoveHardwareCodecFormats(formats);
  }
#endif
  return formats;
}

std::unique_ptr<VideoEncoder> InternalEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
#if (defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || \
     defined(RTC_USE_X265) || defined(RTC_USE_AMF)) && \
    defined(RTC_ENABLE_H265)
  if (IsBackendH265FormatSupported(format)) {
    const std::string preference =
        env.field_trials().Lookup("WebRTC-H265-Encoder");
    const std::string hardware_preference =
        env.field_trials().Lookup("WebRTC-H265-HardwareEncoder");
    // Explicit x265 is independent software intent. Automatic/vendor chains
    // fail closed after any unresolved hardware retirement in this process.
    bool retirement_failed = preference != "x265" && HardwareRetirementFailed();
    auto encoder = external_encoder::CreatePreferredH265Encoder(
        preference, hardware_preference,
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_NVENC)
          return CreateHardwareWithRetirementCheck(
              [&](Status* status) {
                return CreateNvencVideoEncoder(env, format, status);
              },
              retirement_failed);
#else
          return nullptr;
#endif
        },
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_QSV)
          return CreateHardwareWithRetirementCheck(
              [&](Status* status) {
                return CreateQsvVideoEncoder(env, format, status);
              },
              retirement_failed);
#else
          return nullptr;
#endif
        },
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_AMF)
          return CreateHardwareWithRetirementCheck(
              [&](Status* status) {
                return CreateAmfVideoEncoder(env, format, status);
              },
              retirement_failed);
#else
          return nullptr;
#endif
        },
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_X265)
          if (preference != "x265" && HardwareRetirementFailed()) {
            retirement_failed = true;
            return nullptr;
          }
          auto software = CreateX265VideoEncoder(env, format);
          if (preference != "x265" && HardwareRetirementFailed()) {
            retirement_failed = true;
            return nullptr;
          }
          return software;
#else
          return nullptr;
#endif
        },
        &retirement_failed);
    if (retirement_failed && preference != "x265") {
      return nullptr;
    }
    if (encoder) {
      const auto info = encoder->GetEncoderInfo();
      RTC_LOG(LS_INFO) << "H265 encoder requested="
                       << (hardware_preference.empty() ? "auto"
                                                       : hardware_preference)
                       << " effective=" << info.implementation_name;
      if (!info.is_hardware_accelerated && preference != "x265") {
        RTC_LOG(LS_WARNING)
            << "H265 encoder requested=hardware effective="
            << info.implementation_name
            << " fallbackReason=preferred hardware unavailable at creation";
      }
    } else {
      RTC_LOG(LS_WARNING) << "H265 encoder requested=" << hardware_preference
                          << " effective=unavailable fallbackReason=selected "
                             "backend unavailable";
    }
    return encoder;
  }
#endif
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
  if (PayloadStringToCodecType(format.name) == kVideoCodecH264 &&
      HardwareRetirementFailed()) {
    return nullptr;
  }
#endif
  auto original_format =
      FuzzyMatchSdpVideoFormat(Factory().GetSupportedFormats(), format);
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF)
  if (original_format && IsBackendH264FormatSupported(format)) {
    const std::string preference =
        env.field_trials().Lookup("WebRTC-H264-HardwareEncoder");
    bool retirement_failed = false;
    auto encoder = external_encoder::CreatePreferredH264HardwareEncoder(
        preference,
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_NVENC)
          return CreateHardwareWithRetirementCheck(
              [&](Status* status) {
                return CreateNvencVideoEncoder(env, format, status);
              },
              retirement_failed);
#else
          return nullptr;
#endif
        },
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_QSV)
          return CreateHardwareWithRetirementCheck(
              [&](Status* status) {
                return CreateQsvVideoEncoder(env, format, status);
              },
              retirement_failed);
#else
          return nullptr;
#endif
        },
        [&]() -> std::unique_ptr<VideoEncoder> {
#if defined(RTC_USE_AMF)
          return CreateHardwareWithRetirementCheck(
              [&](Status* status) {
                return CreateAmfVideoEncoder(env, format, status);
              },
              retirement_failed);
#else
          return nullptr;
#endif
        },
        &retirement_failed);
    if (retirement_failed || HardwareRetirementFailed()) {
      return nullptr;
    }
    if (encoder) {
      RTC_LOG(LS_INFO) << "H264 encoder requested="
                       << (preference.empty() ? "auto" : preference)
                       << " effective="
                       << encoder->GetEncoderInfo().implementation_name;
      return encoder;
    }
    RTC_LOG(LS_WARNING)
        << "H264 encoder requested="
        << (preference.empty() ? "auto" : preference)
        << " effective=OpenH264 "
           "fallbackReason=requested hardware unavailable at creation";
  }
#endif
  auto software =
      original_format ? Factory().Create(env, *original_format) : nullptr;
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
  if (PayloadStringToCodecType(format.name) == kVideoCodecH264 &&
      HardwareRetirementFailed()) {
    return nullptr;
  }
#endif
  return software;
}

VideoEncoderFactory::CodecSupport InternalEncoderFactory::QueryCodecSupport(
    const SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
  if (IsHardwareCodec(format) && HardwareRetirementFailed()) {
    return {.is_supported = false, .is_power_efficient = false};
  }
#endif
#if (defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || \
     defined(RTC_USE_X265) || defined(RTC_USE_AMF)) && \
    defined(RTC_ENABLE_H265)
  if (IsBackendH265FormatSupported(format)) {
    bool supported = false;
    bool hardware_available = false;
    if (!scalability_mode || *scalability_mode == "L1T1") {
      const Status hardware_status = ProbeHardwareFormat(format);
      if (hardware_status == Status::kRetirementFailed) {
        return {.is_supported = false, .is_power_efficient = false};
      }
      supported = hardware_status == Status::kOk;
      hardware_available = supported;
#if defined(RTC_USE_X265)
      supported = supported || IsX265VideoEncoderAvailable(format);
#endif
      if (HardwareRetirementFailed()) {
        return {.is_supported = false, .is_power_efficient = false};
      }
    }
    return {.is_supported = supported,
            .is_power_efficient = hardware_available};
  }
#endif
  auto original_format =
      FuzzyMatchSdpVideoFormat(Factory().GetSupportedFormats(), format);
  auto support =
      original_format
          ? Factory().QueryCodecSupport(*original_format, scalability_mode)
          : VideoEncoderFactory::CodecSupport{.is_supported = false};
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF)
  if (support.is_supported && original_format &&
      IsBackendH264FormatSupported(format) &&
      (!scalability_mode || *scalability_mode == "L1T1")) {
    const Status hardware_status = ProbeHardwareFormat(format);
    if (hardware_status == Status::kRetirementFailed) {
      return {.is_supported = false, .is_power_efficient = false};
    }
    support.is_power_efficient = hardware_status == Status::kOk;
  }
#endif
#if defined(RTC_USE_NVENC) || defined(RTC_USE_QSV) || defined(RTC_USE_AMF) || \
    defined(RTC_USE_X265)
  if (PayloadStringToCodecType(format.name) == kVideoCodecH264 &&
      HardwareRetirementFailed()) {
    return {.is_supported = false, .is_power_efficient = false};
  }
#endif
  return support;
}

}  // namespace webrtc
