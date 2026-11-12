/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/amf_video_encoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "api/video_codecs/h264_profile_level_id.h"
#if defined(RTC_ENABLE_H265)
#include "api/video_codecs/h265_profile_tier_level.h"
#endif
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#if defined(RTC_USE_X265)
#include "modules/video_coding/codecs/external/x265_video_encoder.h"
#endif
#include "rtc_base/logging.h"

namespace webrtc {

external_encoder::Status ProbeAmfVideoEncoderAvailability(
    const SdpVideoFormat& format,
    std::string* reason) {
#if defined(RTC_ENABLE_H265)
  if (IsBackendH265FormatSupported(format)) {
    const auto ptl = ParseSdpForH265ProfileTierLevel(format.parameters);
    return external_encoder::ProbeAmfH265Backend(
        reason, static_cast<uint8_t>(ptl->level));
  }
#endif
  if (!IsBackendH264FormatSupported(format)) {
    return external_encoder::Status::kUnsupported;
  }
  const auto profile = ParseSdpForH264ProfileLevelId(format.parameters);
  return external_encoder::ProbeAmfH264Backend(
      reason, static_cast<uint8_t>(profile->level));
}

bool IsAmfVideoEncoderAvailable(const SdpVideoFormat& format) {
  std::string reason;
  const auto status = ProbeAmfVideoEncoderAvailability(format, &reason);
  if (status != external_encoder::Status::kOk) {
    RTC_LOG(LS_WARNING) << "AMF " << format.name
                        << " capability unavailable: " << reason;
  }
  return status == external_encoder::Status::kOk;
}

std::unique_ptr<VideoEncoder> CreateAmfVideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format,
    external_encoder::Status* probe_status) {
  std::string reason;
  const auto status = ProbeAmfVideoEncoderAvailability(format, &reason);
  if (probe_status) {
    *probe_status = status;
  }
  if (status != external_encoder::Status::kOk) {
    RTC_LOG(LS_WARNING)
        << format.name
        << " encoder requested=AMF effective=unavailable "
           "fallbackReason=format or actual AMD hardware initialization probe "
        << reason;
    return nullptr;
  }
  if (IsBackendH265FormatSupported(format)) {
#if defined(RTC_ENABLE_H265)
#if defined(RTC_USE_X265)
    constexpr auto failure_policy =
        BackendFailurePolicy::kSameCodecSoftwareFallback;
#else
    constexpr auto failure_policy = BackendFailurePolicy::kDefault;
#endif
    auto hardware = CreateBackendVideoEncoder(
        format, external_encoder::CreateAmfH265Backend(), failure_policy);
#if defined(RTC_USE_X265)
    auto software = CreateX265VideoEncoder(env, format);
    if (!hardware || !software) {
      RTC_LOG(LS_WARNING)
          << "H265 encoder requested=AMF effective=unavailable "
             "fallbackReason=adapter or x265 construction failed";
      return nullptr;
    }
    RTC_LOG(LS_INFO) << "H265 encoder requested=AMF effective="
                     << hardware->GetEncoderInfo().implementation_name
                     << " runtimeFallback="
                     << software->GetEncoderInfo().implementation_name;
    return CreateVideoEncoderSoftwareFallbackWrapper(
        env, std::move(software), std::move(hardware),
        /*prefer_temporal_support=*/false);
#else
    RTC_LOG(LS_INFO) << "H265 encoder requested=AMF effective=AMF H265 "
                        "runtimeFallback=unavailable (x265 disabled)";
    return hardware;
#endif
#else
    return nullptr;
#endif
  }
  auto hardware = CreateBackendVideoEncoder(
      format, external_encoder::CreateAmfH264Backend());
  auto software = CreateH264Encoder(env, H264EncoderSettings::Parse(format));
  if (!hardware || !software) {
    RTC_LOG(LS_WARNING)
        << "H264 encoder requested=AMF effective=unavailable "
           "fallbackReason=adapter or OpenH264 construction failed";
    return nullptr;
  }
  RTC_LOG(LS_INFO) << "H264 encoder requested=AMF effective="
                   << hardware->GetEncoderInfo().implementation_name
                   << " runtimeFallback="
                   << software->GetEncoderInfo().implementation_name;
  return CreateVideoEncoderSoftwareFallbackWrapper(
      env, std::move(software), std::move(hardware),
      /*prefer_temporal_support=*/false);
}

}  // namespace webrtc
