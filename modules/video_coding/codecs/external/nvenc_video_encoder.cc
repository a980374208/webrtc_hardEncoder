/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/nvenc_video_encoder.h"

#include <memory>
#include <utility>
#if defined(RTC_ENABLE_H265)
#include "api/video_codecs/h265_profile_tier_level.h"
#endif

#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#if defined(RTC_USE_X265)
#include "modules/video_coding/codecs/external/x265_video_encoder.h"
#endif
#include "rtc_base/logging.h"

namespace webrtc {

external_encoder::Status ProbeNvencVideoEncoderAvailability(
    const SdpVideoFormat& format,
    std::string* reason) {
  if (IsNvencH265FormatSupported(format)) {
#if defined(RTC_ENABLE_H265)
    const auto ptl = ParseSdpForH265ProfileTierLevel(format.parameters);
    return external_encoder::ProbeNvencH265Backend(
        reason, static_cast<uint8_t>(ptl->level));
#endif
  }
  return IsNvencH264FormatSupported(format)
             ? external_encoder::ProbeNvencH264Backend(reason)
             : external_encoder::Status::kUnsupported;
}

bool IsNvencVideoEncoderAvailable(const SdpVideoFormat& format) {
  return ProbeNvencVideoEncoderAvailability(format) ==
         external_encoder::Status::kOk;
}

std::unique_ptr<VideoEncoder> CreateNvencVideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format,
    external_encoder::Status* probe_status) {
  const auto status = ProbeNvencVideoEncoderAvailability(format);
  if (probe_status) {
    *probe_status = status;
  }
  if (status != external_encoder::Status::kOk) {
    return nullptr;
  }
  if (IsNvencH265FormatSupported(format)) {
#if defined(RTC_ENABLE_H265)
#if defined(RTC_USE_X265)
    constexpr auto failure_policy =
        BackendFailurePolicy::kSameCodecSoftwareFallback;
#else
    constexpr auto failure_policy = BackendFailurePolicy::kDefault;
#endif
    auto hardware = CreateBackendVideoEncoder(
        format, external_encoder::CreateNvencH265Backend(), failure_policy);
#if defined(RTC_USE_X265)
    auto software = CreateX265VideoEncoder(env, format);
    if (!hardware || !software) {
      RTC_LOG(LS_WARNING)
          << "H265 encoder requested=NVENC effective=unavailable "
             "fallbackReason=adapter or x265 construction failed";
      return nullptr;
    }
    RTC_LOG(LS_INFO) << "H265 encoder requested=NVENC effective="
                     << hardware->GetEncoderInfo().implementation_name
                     << " runtimeFallback="
                     << software->GetEncoderInfo().implementation_name;
    return CreateVideoEncoderSoftwareFallbackWrapper(
        env, std::move(software), std::move(hardware),
        /*prefer_temporal_support=*/false);
#else
    RTC_LOG(LS_INFO)
        << "H265 encoder requested=NVENC effective=NVENC H265/D3D11 "
           "runtimeFallback=unavailable (x265 disabled)";
    return hardware;
#endif
#else
    return nullptr;
#endif
  }
  return CreateVideoEncoderSoftwareFallbackWrapper(
      env, CreateH264Encoder(env, H264EncoderSettings::Parse(format)),
      CreateBackendVideoEncoder(format,
                                external_encoder::CreateNvencH264Backend()),
      /*prefer_temporal_support=*/false);
}

}  // namespace webrtc
