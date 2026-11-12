/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_HEVC_DIMENSIONS_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_HEVC_DIMENSIONS_H_

#include <cstdint>
#include <optional>

#include "api/array_view.h"
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_sps_parser.h"
#include "rtc_base/bitstream_reader.h"

namespace webrtc::external_encoder {

struct QsvHevcDimensions {
  uint32_t coded_width;
  uint32_t coded_height;
  uint32_t display_width;
  uint32_t display_height;
};

// Parse one Annex B SPS for the Main, 8-bit, single-layer dimension contract.
// The common parser intentionally reads only the SPS fields it uses; complete
// bitstream validity remains the adapter/reference decoder's responsibility.
inline std::optional<QsvHevcDimensions> ParseQsvHevcSpsDimensions(
    ArrayView<const uint8_t> annexb) {
  const auto nalus = H265::FindNaluIndices(annexb);
  if (nalus.size() != 1 || nalus[0].start_offset != 0 ||
      nalus[0].payload_size <= H265::kNaluHeaderSize ||
      nalus[0].payload_start_offset + nalus[0].payload_size != annexb.size()) {
    return std::nullopt;
  }
  const auto payload =
      annexb.subview(nalus[0].payload_start_offset, nalus[0].payload_size);
  // SPS type, forbidden_zero_bit=0, layer 0, temporal_id_plus1=1.
  if (payload[0] != 0x42 || payload[1] != 1) {
    return std::nullopt;
  }
  const auto data = payload.subview(H265::kNaluHeaderSize);
  const auto parsed = H265SpsParser::ParseSps(data);
  if (!parsed || parsed->chroma_format_idc != 1 ||
      parsed->bit_depth_luma_minus8 != 0 ||
      parsed->sps_max_sub_layers_minus1 != 0) {
    return std::nullopt;
  }

  // SpsState does not expose chroma bit depth or profile space/tier. Read the
  // same SPS prefix as the adapter without changing the shared parser API.
  const auto rbsp = H265::ParseRbsp(data);
  BitstreamReader reader(rbsp);
  reader.ConsumeBits(4);  // sps_video_parameter_set_id.
  const uint32_t max_sub_layers = reader.ReadBits(3);
  reader.ConsumeBits(1);  // sps_temporal_id_nesting_flag.
  BitstreamReader tier_reader(rbsp);
  tier_reader.ConsumeBits(8);
  const uint32_t profile_space = tier_reader.ReadBits(2);
  const bool high_tier = tier_reader.Read<bool>();
  const auto ptl = H265SpsParser::ParseProfileTierLevel(true, 0, reader);
  const bool reader_ok = reader.Ok();
  const bool tier_reader_ok = tier_reader.Ok();
  if (!reader_ok || max_sub_layers != 0 || !tier_reader_ok ||
      profile_space != 0 || high_tier || !ptl ||
      ptl->general_profile_idc != 1) {
    return std::nullopt;
  }
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
    return std::nullopt;
  }

  QsvHevcDimensions dimensions = {parsed->pic_width_in_luma_samples,
                                  parsed->pic_height_in_luma_samples,
                                  parsed->width, parsed->height};
  if (!dimensions.coded_width || !dimensions.coded_height ||
      !dimensions.display_width || !dimensions.display_height ||
      dimensions.coded_width > 8192 || dimensions.coded_height > 8192 ||
      dimensions.display_width > dimensions.coded_width ||
      dimensions.display_height > dimensions.coded_height) {
    return std::nullopt;
  }
  return dimensions;
}

// SDK HEVC dimensions describe the coded picture. FrameInfo crop and the
// decoder's visible dimensions describe the requested display picture.
inline bool QsvHevcDimensionsMatch(const QsvHevcDimensions& dimensions,
                                   uint32_t requested_width,
                                   uint32_t requested_height,
                                   uint32_t storage_width,
                                   uint32_t storage_height,
                                   uint32_t sdk_coded_width,
                                   uint32_t sdk_coded_height) {
  return dimensions.coded_width > 0 && dimensions.coded_height > 0 &&
         dimensions.display_width > 0 && dimensions.display_height > 0 &&
         dimensions.coded_width <= 8192 && dimensions.coded_height <= 8192 &&
         dimensions.display_width <= dimensions.coded_width &&
         dimensions.display_height <= dimensions.coded_height &&
         dimensions.display_width == requested_width &&
         dimensions.display_height == requested_height &&
         dimensions.coded_width == sdk_coded_width &&
         dimensions.coded_height == sdk_coded_height && storage_width <= 8192 &&
         storage_height <= 8192 && dimensions.coded_width <= storage_width &&
         dimensions.coded_height <= storage_height &&
         requested_width <= storage_width && requested_height <= storage_height;
}

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_HEVC_DIMENSIONS_H_
