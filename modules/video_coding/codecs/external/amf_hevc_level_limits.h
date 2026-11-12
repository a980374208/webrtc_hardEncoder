/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_AMF_HEVC_LEVEL_LIMITS_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_AMF_HEVC_LEVEL_LIMITS_H_

#include <array>
#include <cstdint>

namespace webrtc::external_encoder {

struct AmfHevcLevelLimits {
  uint8_t level_idc;
  uint64_t max_luma_picture_size;
  uint64_t max_luma_sample_rate;
  uint32_t max_main_tier_bitrate_bps;
};

// HEVC Annex A.4.1 and Tables A.8/A.9: Main profile, Main tier. Picture/sample
// limits agree with the existing OBS AMF table; bitrate is the Main VCL limit,
// so we do not spend any NAL-overhead allowance as configured encoder bitrate.
inline const AmfHevcLevelLimits* FindAmfHevcLevelLimits(uint8_t level_idc) {
  static constexpr std::array<AmfHevcLevelLimits, 13> levels = {{
      {30, 36864, 552960, 128000},
      {60, 122880, 3686400, 1500000},
      {63, 245760, 7372800, 3000000},
      {90, 552960, 16588800, 6000000},
      {93, 983040, 33177600, 10000000},
      {120, 2228224, 66846720, 12000000},
      {123, 2228224, 133693440, 20000000},
      {150, 8912896, 267386880, 25000000},
      {153, 8912896, 534773760, 40000000},
      {156, 8912896, 1069547520, 60000000},
      {180, 35651584, 1069547520, 60000000},
      {183, 35651584, 2139095040, 120000000},
      {186, 35651584, 4278190080, 240000000},
  }};
  for (const auto& level : levels) {
    if (level.level_idc == level_idc) {
      return &level;
    }
  }
  return nullptr;
}

// Pass the actual SPS coded dimensions, including padding outside the display
// crop. Before AMF Init, visible dimensions may only be used as a lower-bound
// rejection; the emitted SPS must still pass this gate independently.
// nullptr means valid. All products are bounded or divided before comparison
// so even corrupt 32-bit SPS dimensions cannot wrap into a successful result.
inline const char* AmfHevcLevelViolation(uint8_t level_idc,
                                         uint32_t coded_width,
                                         uint32_t coded_height,
                                         uint32_t framerate_fps,
                                         uint32_t bitrate_bps) {
  const auto* level = FindAmfHevcLevelLimits(level_idc);
  if (!level) {
    return "unknown HEVC level";
  }
  if (!coded_width || !coded_height || !framerate_fps || !bitrate_bps) {
    return "zero HEVC coded dimension/framerate/bitrate";
  }
  const uint64_t width = coded_width;
  const uint64_t height = coded_height;
  const uint64_t pixels = width * height;
  if (pixels > level->max_luma_picture_size) {
    return "HEVC coded picture exceeds MaxLumaPs";
  }
  if (width * width > 8 * level->max_luma_picture_size ||
      height * height > 8 * level->max_luma_picture_size) {
    return "HEVC coded dimension exceeds sqrt(8*MaxLumaPs)";
  }
  if (framerate_fps > level->max_luma_sample_rate / pixels) {
    return "HEVC coded picture/framerate exceeds MaxLumaSr";
  }
  if (bitrate_bps > level->max_main_tier_bitrate_bps) {
    return "HEVC configured bitrate exceeds Main-tier MaxBR";
  }
  return nullptr;
}

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_AMF_HEVC_LEVEL_LIMITS_H_
