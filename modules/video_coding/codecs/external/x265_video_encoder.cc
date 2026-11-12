/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/x265_video_encoder.h"

#include <cstdint>

#include "api/video_codecs/h265_profile_tier_level.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/x265_video_encoder_backend.h"
namespace webrtc {
bool IsX265VideoEncoderAvailable(const SdpVideoFormat& format) {
  if (!IsBackendH265FormatSupported(format))
    return false;
  const auto ptl = ParseSdpForH265ProfileTierLevel(format.parameters);
  return ptl && external_encoder::ProbeX265H265Backend(
                    nullptr, static_cast<uint8_t>(ptl->level)) ==
                    external_encoder::Status::kOk;
}
std::unique_ptr<VideoEncoder> CreateX265VideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format) {
  const auto tuning = external_encoder::ParseX265ExperimentalTuning(
      env.field_trials().Lookup("WebRTC-X265-ExperimentalTuning"));
  if (!tuning)
    return nullptr;
  if (!IsX265VideoEncoderAvailable(format))
    return nullptr;
  return CreateBackendVideoEncoder(
      format, external_encoder::CreateX265H265Backend(*tuning));
}
}  // namespace webrtc
