/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_H265_ENCODER_POLICY_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_H265_ENCODER_POLICY_H_
#include <string_view>
#include <type_traits>
#include <utility>

#include "modules/video_coding/codecs/external/hardware_h264_encoder_policy.h"
namespace webrtc::external_encoder {
// Creation-time selection. Explicit hardware retains its vendor requirement;
// explicit x265 selects software before hardware probes. Same-codec runtime
// fallback is controlled by each selected vendor's wrapper.
template <typename NvencCreator, typename QsvCreator, typename X265Creator>
auto CreatePreferredH265Encoder(std::string_view encoder_preference,
                                std::string_view hardware_preference,
                                NvencCreator nvenc,
                                QsvCreator qsv,
                                X265Creator x265,
                                const bool* retirement_failed = nullptr)
    -> decltype(nvenc()) {
  // Explicit software is an independent user choice, not a fallback after
  // failed hardware retirement, and never initializes a hardware backend.
  if (encoder_preference == "x265")
    return x265();
  auto encoder = CreatePreferredHardwareEncoder(
      hardware_preference, std::move(nvenc), std::move(qsv), retirement_failed);
  if (retirement_failed && *retirement_failed)
    return nullptr;
  if (encoder || hardware_preference == "NVENC" || hardware_preference == "QSV")
    return encoder;
  encoder = x265();
  return retirement_failed && *retirement_failed ? nullptr : std::move(encoder);
}

// AMF extends the automatic hardware order, while explicit vendors stay
// isolated. Software selection remains HEVC and is never a codec switch.
template <typename NvencCreator,
          typename QsvCreator,
          typename AmfCreator,
          typename X265Creator>
auto CreatePreferredH265Encoder(std::string_view encoder_preference,
                                std::string_view hardware_preference,
                                NvencCreator nvenc,
                                QsvCreator qsv,
                                AmfCreator amf,
                                X265Creator x265,
                                const bool* retirement_failed = nullptr)
    -> std::enable_if_t<std::is_invocable_v<X265Creator>, decltype(nvenc())> {
  if (encoder_preference == "x265")
    return x265();
  auto encoder = CreatePreferredH264HardwareEncoder(
      hardware_preference, std::move(nvenc), std::move(qsv), std::move(amf),
      retirement_failed);
  if (retirement_failed && *retirement_failed)
    return nullptr;
  if (encoder || hardware_preference == "NVENC" ||
      hardware_preference == "QSV" || hardware_preference == "AMF" ||
      hardware_preference == "AMD")
    return encoder;
  encoder = x265();
  return retirement_failed && *retirement_failed ? nullptr : std::move(encoder);
}
}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_H265_ENCODER_POLICY_H_
