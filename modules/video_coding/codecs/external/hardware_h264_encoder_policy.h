/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_HARDWARE_H264_ENCODER_POLICY_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_HARDWARE_H264_ENCODER_POLICY_H_

#include <string_view>
#include <type_traits>
#include <utility>

namespace webrtc::external_encoder {

// Creation-time preference only. A later runtime failure does not select
// another vendor. Each chosen encoder retains its codec's fallback behavior.
template <typename NvencCreator, typename QsvCreator>
auto CreatePreferredHardwareEncoder(std::string_view preference,
                                    NvencCreator nvenc,
                                    QsvCreator qsv,
                                    const bool* retirement_failed = nullptr)
    -> decltype(nvenc()) {
  const auto blocked = [retirement_failed] {
    return retirement_failed && *retirement_failed;
  };
  if (blocked()) {
    return nullptr;
  }
  if (preference == "QSV") {
    auto encoder = qsv();
    return blocked() ? nullptr : std::move(encoder);
  }
  if (preference == "NVENC") {
    auto encoder = nvenc();
    return blocked() ? nullptr : std::move(encoder);
  }
  auto encoder = nvenc();
  if (blocked()) {
    return nullptr;
  }
  if (!encoder) {
    encoder = qsv();
  }
  return blocked() ? nullptr : std::move(encoder);
}

template <typename NvencCreator, typename QsvCreator>
auto CreatePreferredH264HardwareEncoder(std::string_view preference,
                                        NvencCreator nvenc,
                                        QsvCreator qsv,
                                        const bool* retirement_failed = nullptr)
    -> decltype(nvenc()) {
  return CreatePreferredHardwareEncoder(preference, std::move(nvenc),
                                        std::move(qsv), retirement_failed);
}

// Explicit vendors are isolated. Add AMF to the automatic preference only;
// unknown preferences retain the existing NVENC/QSV selection semantics.
template <typename NvencCreator, typename QsvCreator, typename AmfCreator>
auto CreatePreferredH264HardwareEncoder(std::string_view preference,
                                        NvencCreator nvenc,
                                        QsvCreator qsv,
                                        AmfCreator amf,
                                        const bool* retirement_failed = nullptr)
    -> std::enable_if_t<std::is_invocable_v<AmfCreator>, decltype(nvenc())> {
  const auto blocked = [retirement_failed] {
    return retirement_failed && *retirement_failed;
  };
  if (blocked()) {
    return nullptr;
  }
  if (preference == "AMF" || preference == "AMD") {
    auto encoder = amf();
    return blocked() ? nullptr : std::move(encoder);
  }
  auto encoder = CreatePreferredHardwareEncoder(
      preference, std::move(nvenc), std::move(qsv), retirement_failed);
  if (blocked()) {
    return nullptr;
  }
  if (encoder ||
      (!preference.empty() && preference != "auto" && preference != "Auto")) {
    return encoder;
  }
  encoder = amf();
  return blocked() ? nullptr : std::move(encoder);
}

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_HARDWARE_H264_ENCODER_POLICY_H_
