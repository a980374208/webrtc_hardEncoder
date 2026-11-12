/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "AMF/components/ComponentCaps.h"
#include "AMF/components/VideoEncoderVCE.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#if defined(RTC_ENABLE_H265)
#include "AMF/components/VideoEncoderHEVC.h"
#endif
#include "AMF/core/Buffer.h"
#include "AMF/core/Factory.h"
#include "AMF/core/PropertyStorageEx.h"
#include "api/array_view.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/pps_parser.h"
#include "common_video/h264/sps_parser.h"
#if defined(RTC_ENABLE_H265)
#include "common_video/h265/h265_common.h"
#include "common_video/h265/h265_pps_parser.h"
#include "common_video/h265/h265_sps_parser.h"
#endif
#include "modules/video_coding/codecs/external/amf_hevc_level_limits.h"
#include "rtc_base/bitstream_reader.h"
#include "rtc_base/logging.h"

namespace webrtc::external_encoder {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kAmdVendor = 0x1002;
constexpr size_t kInputSlots = 4;
constexpr size_t kMaximumPacketBytes = 64 * 1024 * 1024;
constexpr auto kPollTimeout = std::chrono::seconds(2);
// OBS's AMF submission path uses this property to preserve caller timestamps
// separately from AMF's 100 ns presentation clock. AMF propagates input props.
constexpr wchar_t kFrameIdProperty[] = L"PTS";

std::string Utf8(const wchar_t* value) {
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr,
                      nullptr);
  result.pop_back();
  return result;
}

std::string AmfFailure(const char* operation, AMF_RESULT result) {
  return std::string(operation) +
         " failed, AMF_RESULT=" + std::to_string(static_cast<int>(result));
}

bool ValidLevel(uint8_t level) {
  constexpr std::array<uint8_t, 19> levels = {10, 11, 12, 13, 20, 21, 22,
                                              30, 31, 32, 40, 41, 42, 50,
                                              51, 52, 60, 61, 62};
  for (const auto supported : levels) {
    if (level == supported) {
      return true;
    }
  }
  return false;
}

bool ValidRates(const Rates& rates) {
  return rates.bitrate_bps > 0 && rates.bitrate_bps <= 65535000 &&
         rates.framerate_fps > 0 && rates.framerate_fps <= 240;
}

bool ValidH265Level(uint8_t level) {
  return FindAmfHevcLevelLimits(level) != nullptr;
}

bool ValidConfig(const Config& config, Codec codec) {
  return config.codec == codec && config.width >= 16 && config.height >= 16 &&
         config.width <= 8192 && config.height <= 8192 &&
         config.width % 2 == 0 && config.height % 2 == 0 &&
         (codec == Codec::kH264 ? ValidLevel(config.h264_level)
                                : ValidH265Level(config.h265_level)) &&
         ValidRates({config.bitrate_bps, config.framerate_fps});
}

bool HasNal(ArrayView<const uint8_t> bytes, H264::NaluType type) {
  for (const auto& index : H264::FindNaluIndices(bytes)) {
    if (index.payload_size &&
        H264::ParseNaluType(bytes[index.payload_start_offset]) == type) {
      return true;
    }
  }
  return false;
}

bool AnnexB(ArrayView<const uint8_t> bytes) {
  const auto indices = H264::FindNaluIndices(bytes);
  if (indices.empty() || indices.front().start_offset != 0 ||
      indices.back().payload_start_offset + indices.back().payload_size !=
          bytes.size()) {
    return false;
  }
  for (const auto& index : indices) {
    if (!index.payload_size ||
        (bytes[index.payload_start_offset] & 0x80) != 0) {
      return false;
    }
    const auto type = H264::ParseNaluType(bytes[index.payload_start_offset]);
    if (type != H264::kSlice && type != H264::kIdr && type != H264::kSei &&
        type != H264::kSps && type != H264::kPps && type != H264::kAud &&
        type != H264::kFiller) {
      return false;
    }
  }
  return true;
}

// Parse the emitted parameter sets, independently of property readback. The
// profile byte enforces Baseline's 8-bit 4:2:0 syntax; dimensions include crop.
bool ValidateHeaders(ArrayView<const uint8_t> bytes,
                     const Config& config,
                     uint32_t* pps_id) {
  std::optional<SpsParser::SpsState> sps;
  std::optional<PpsParser::PpsState> pps;
  for (const auto& index : H264::FindNaluIndices(bytes)) {
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    if (nal.empty()) {
      return false;
    }
    const auto type = H264::ParseNaluType(nal[0]);
    if (type == H264::kSps) {
      if (sps || nal.size() < 4 || nal[1] != 66 || !ValidLevel(nal[3]) ||
          nal[3] > config.h264_level) {
        return false;
      }
      sps = SpsParser::ParseSps(nal.subview(1));
      if (!sps || sps->width != static_cast<uint32_t>(config.width) ||
          sps->height != static_cast<uint32_t>(config.height) ||
          sps->chroma_format_idc != 1 || !sps->frame_mbs_only_flag ||
          sps->max_num_ref_frames > 1) {
        return false;
      }
    } else if (type == H264::kPps) {
      if (pps) {
        return false;
      }
      pps = PpsParser::ParsePps(nal.subview(1));
      if (!pps || pps->entropy_coding_mode_flag ||
          pps->num_ref_idx_l0_default_active_minus1 != 0) {
        return false;
      }
    }
  }
  if (!sps || !pps || pps->sps_id != sps->id) {
    return false;
  }
  *pps_id = pps->id;
  return true;
}

bool ValidatePicture(ArrayView<const uint8_t> bytes,
                     bool idr,
                     uint32_t expected_pps_id) {
  int slices = 0;
  for (const auto& index : H264::FindNaluIndices(bytes)) {
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    const auto type = H264::ParseNaluType(nal[0]);
    if (type != H264::kSlice && type != H264::kIdr) {
      continue;
    }
    if (++slices != 1 || nal.size() <= 1 || (type == H264::kIdr) != idr) {
      return false;
    }
    const auto rbsp = H264::ParseRbsp(nal.subview(1));
    BitstreamReader reader(rbsp);
    const auto first_mb = reader.ReadExponentialGolomb();
    const auto slice_type = reader.ReadExponentialGolomb();
    const auto pps_id = reader.ReadExponentialGolomb();
    if (!reader.Ok() || first_mb != 0 || slice_type > 9 ||
        pps_id != expected_pps_id ||
        (slice_type % 5 != H264::kP && slice_type % 5 != H264::kI) ||
        (idr && slice_type % 5 != H264::kI)) {
      return false;
    }
  }
  return slices == 1;
}

#if defined(RTC_ENABLE_H265)
bool HasH265Nal(ArrayView<const uint8_t> bytes, H265::NaluType type) {
  for (const auto& index : H265::FindNaluIndices(bytes)) {
    if (index.payload_size >= 2 &&
        H265::ParseNaluType(bytes[index.payload_start_offset]) == type) {
      return true;
    }
  }
  return false;
}

bool RejectH265(std::string* reason, const std::string& message) {
  RTC_LOG(LS_WARNING) << "AMF_HEVC_REJECT " << message;
  if (reason) {
    *reason = message;
  }
  return false;
}

bool H265AnnexB(ArrayView<const uint8_t> bytes, std::string* reason = nullptr) {
  const auto indices = H265::FindNaluIndices(bytes);
  if (indices.empty() || indices.front().start_offset != 0 ||
      indices.back().payload_start_offset + indices.back().payload_size !=
          bytes.size()) {
    std::ostringstream details;
    details << "AnnexB framing nals=" << indices.size()
            << " bytes=" << bytes.size();
    if (!indices.empty()) {
      details << " firstStart=" << indices.front().start_offset
              << " payloadEnd="
              << indices.back().payload_start_offset +
                     indices.back().payload_size;
    }
    return RejectH265(reason, details.str());
  }
  for (const auto& index : indices) {
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    // Only one layer and temporal_id_plus1=1. A successful API call is not
    // sufficient to accept reordered or multilayer output.
    if (nal.size() < 2 || (nal[0] & 0x81) != 0 || nal[1] != 1) {
      std::ostringstream details;
      details << "NAL header bytes=" << nal.size();
      if (nal.size() >= 2) {
        details << " type=" << static_cast<int>(H265::ParseNaluType(nal[0]))
                << " forbiddenZeroBit=" << ((nal[0] >> 7) & 1)
                << " layerId=" << (((nal[0] & 1) << 5) | (nal[1] >> 3))
                << " temporalIdPlus1=" << (nal[1] & 7);
      }
      return RejectH265(reason, details.str());
    }
    const auto type = H265::ParseNaluType(nal[0]);
    if (type != H265::kTrailN && type != H265::kTrailR &&
        type != H265::kIdrWRadl && type != H265::kIdrNLp &&
        type != H265::kVps && type != H265::kSps && type != H265::kPps &&
        type != H265::kAud && type != H265::kPrefixSei &&
        type != H265::kSuffixSei && type != 38 /* filler_data */) {
      return RejectH265(reason, "unexpected NAL type=" +
                                    std::to_string(static_cast<int>(type)));
    }
  }
  return true;
}

bool ValidateH265Ptl(BitstreamReader& reader,
                     const Config& config,
                     std::string_view parameter_set,
                     std::string* reason,
                     uint8_t* emitted_level) {
  const auto space = reader.ReadBits(2);
  const bool high_tier = reader.Read<bool>();
  const auto profile = reader.ReadBits(5);
  reader.ConsumeBits(32);  // general_profile_compatibility_flags.
  reader.ConsumeBits(1);   // general_progressive_source_flag.
  const bool interlaced = reader.Read<bool>();
  reader.ConsumeBits(46);  // Remaining source/constraint flags.
  const auto level = reader.ReadBits(8);
  if (emitted_level && reader.Ok()) {
    *emitted_level = static_cast<uint8_t>(level);
  }
  const bool valid =
      reader.Ok() && space == 0 && !high_tier && profile == 1 && !interlaced &&
      ValidH265Level(static_cast<uint8_t>(level)) && level <= config.h265_level;
  if (!valid) {
    std::ostringstream details;
    details << parameter_set << " PTL parseOk=" << reader.Ok()
            << " space=" << space << " highTier=" << high_tier
            << " profile=" << profile << " interlaced=" << interlaced
            << " level=" << level
            << " requestedLevel=" << static_cast<int>(config.h265_level);
    return RejectH265(reason, details.str());
  }
  return valid;
}

// Parse the VPS syntax directly: the common VPS parser in this checkout does
// not retain vps_max_layers_minus1 and the actual sublayer field separately.
bool ValidateH265Vps(ArrayView<const uint8_t> data,
                     const Config& config,
                     uint32_t* vps_id,
                     uint8_t* level_idc,
                     std::string* reason) {
  const auto rbsp = H265::ParseRbsp(data);
  BitstreamReader reader(rbsp);
  *vps_id = reader.ReadBits(4);
  const bool internal = reader.Read<bool>();
  const bool available = reader.Read<bool>();
  const auto max_layers = reader.ReadBits(6);
  const auto max_sublayers = reader.ReadBits(3);
  const bool nesting = reader.Read<bool>();
  const auto reserved = reader.ReadBits(16);
  if (!reader.Ok() || !internal || !available || max_layers != 0 ||
      max_sublayers != 0 || !nesting || reserved != 0xffff) {
    std::ostringstream details;
    details << "VPS header parseOk=" << reader.Ok() << " vpsId=" << *vps_id
            << " layersMinus1=" << max_layers
            << " sublayersMinus1=" << max_sublayers << " nesting=" << nesting
            << " internal=" << internal << " available=" << available
            << " reserved=" << reserved;
    return RejectH265(reason, details.str());
  }
  if (!ValidateH265Ptl(reader, config, "VPS", reason, level_idc)) {
    return false;
  }
  reader.ConsumeBits(1);  // vps_sub_layer_ordering_info_present_flag.
  const auto buffering = reader.ReadExponentialGolomb();
  const auto reorder = reader.ReadExponentialGolomb();
  const auto latency = reader.ReadExponentialGolomb();
  const auto max_layer_id = reader.ReadBits(6);
  const auto layer_sets = reader.ReadExponentialGolomb();
  const bool valid = reader.Ok() && buffering <= 1 && reorder == 0 &&
                     max_layer_id == 0 && layer_sets == 0;
  if (!valid) {
    std::ostringstream details;
    details << "VPS order parseOk=" << reader.Ok()
            << " level=" << static_cast<int>(*level_idc)
            << " bufferingMinus1=" << buffering << " reorder=" << reorder
            << " latencyPlus1=" << latency << " maxLayerId=" << max_layer_id
            << " layerSetsMinus1=" << layer_sets;
    return RejectH265(reason, details.str());
  }
  return valid;
}

bool ValidateH265Sps(ArrayView<const uint8_t> data,
                     const Config& config,
                     std::string* reason,
                     std::string* description) {
  const auto rbsp = H265::ParseRbsp(data);
  BitstreamReader reader(rbsp);
  const auto vps_id = reader.ReadBits(4);
  const auto sublayers = reader.ReadBits(3);
  const bool nesting = reader.Read<bool>();
  uint8_t level_idc = 0;
  if (!reader.Ok() || sublayers != 0 || !nesting) {
    std::ostringstream details;
    details << "SPS header parseOk=" << reader.Ok() << " vpsId=" << vps_id
            << " sublayersMinus1=" << sublayers << " nesting=" << nesting;
    return RejectH265(reason, details.str());
  }
  if (!ValidateH265Ptl(reader, config, "SPS", reason, &level_idc)) {
    return false;
  }
  const auto sps_id = reader.ReadExponentialGolomb();
  const auto chroma = reader.ReadExponentialGolomb();
  if (!reader.Ok() || chroma != 1) {
    std::ostringstream details;
    details << "SPS chroma parseOk=" << reader.Ok() << " spsId=" << sps_id
            << " chroma=" << chroma;
    return RejectH265(reason, details.str());
  }
  const auto coded_width = reader.ReadExponentialGolomb();
  const auto coded_height = reader.ReadExponentialGolomb();
  std::array<uint32_t, 4> crop{};
  const bool conformance_window = reader.Read<bool>();
  if (conformance_window) {
    for (auto& offset : crop) {
      offset = reader.ReadExponentialGolomb();
    }
  }
  const auto luma_depth = reader.ReadExponentialGolomb();
  const auto chroma_depth = reader.ReadExponentialGolomb();
  reader.ReadExponentialGolomb();  // log2_max_pic_order_cnt_lsb_minus4.
  reader.ConsumeBits(1);           // sub_layer_ordering_info_present_flag.
  const auto buffering = reader.ReadExponentialGolomb();
  const auto reorder = reader.ReadExponentialGolomb();
  const auto latency = reader.ReadExponentialGolomb();
  std::ostringstream details;
  details << "SPS prefix parseOk=" << reader.Ok() << " vpsId=" << vps_id
          << " spsId=" << sps_id << " level=" << static_cast<int>(level_idc)
          << " coded=" << coded_width << "x" << coded_height
          << " cropFlag=" << conformance_window << " cropUnits=" << crop[0]
          << "," << crop[1] << "," << crop[2] << "," << crop[3]
          << " expectedDisplay=" << config.width << "x" << config.height
          << " lumaDepthMinus8=" << luma_depth
          << " chromaDepthMinus8=" << chroma_depth
          << " bufferingMinus1=" << buffering << " reorder=" << reorder
          << " latencyPlus1=" << latency;
  *description = details.str();
  const bool valid = reader.Ok() && luma_depth == 0 && chroma_depth == 0 &&
                     buffering <= 1 && reorder == 0;
  if (!valid) {
    return RejectH265(reason, *description);
  }
  const char* violation =
      AmfHevcLevelViolation(level_idc, coded_width, coded_height,
                            config.framerate_fps, config.bitrate_bps);
  if (violation) {
    details << " violation=" << violation << " fps=" << config.framerate_fps
            << " bitrate=" << config.bitrate_bps;
    return RejectH265(reason, details.str());
  }
  return true;
}

bool ValidateH265Headers(ArrayView<const uint8_t> bytes,
                         const Config& config,
                         H265PpsParser::PpsState* state,
                         std::string* reason) {
  std::optional<uint32_t> vps;
  uint8_t vps_level = 0;
  std::optional<H265SpsParser::SpsState> sps;
  std::optional<H265PpsParser::PpsState> pps;
  std::string sps_description;
  ArrayView<const uint8_t> pps_data;
  for (const auto& index : H265::FindNaluIndices(bytes)) {
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    if (nal.size() <= 2) {
      return RejectH265(reason, "parameter-set NAL too short; bytes=" +
                                    std::to_string(nal.size()));
    }
    const auto type = H265::ParseNaluType(nal[0]);
    if (type == H265::kVps) {
      uint32_t id;
      if (vps) {
        return RejectH265(reason, "duplicate VPS");
      }
      if (!ValidateH265Vps(nal.subview(2), config, &id, &vps_level, reason)) {
        return false;
      }
      vps = id;
    } else if (type == H265::kSps) {
      if (sps) {
        return RejectH265(reason, "duplicate SPS");
      }
      if (!ValidateH265Sps(nal.subview(2), config, reason, &sps_description)) {
        return false;
      }
      sps = H265SpsParser::ParseSps(nal.subview(2));
      if (!sps) {
        return RejectH265(reason,
                          "SPS common parser rejected; " + sps_description);
      }
      if (sps->chroma_format_idc != 1 || sps->sps_max_sub_layers_minus1 != 0 ||
          sps->bit_depth_luma_minus8 != 0 ||
          sps->width != static_cast<uint32_t>(config.width) ||
          sps->height != static_cast<uint32_t>(config.height) ||
          sps->width > sps->pic_width_in_luma_samples ||
          sps->height > sps->pic_height_in_luma_samples) {
        std::ostringstream details;
        details << "SPS parsed contract display=" << sps->width << "x"
                << sps->height << " coded=" << sps->pic_width_in_luma_samples
                << "x" << sps->pic_height_in_luma_samples
                << " chroma=" << sps->chroma_format_idc
                << " sublayersMinus1=" << sps->sps_max_sub_layers_minus1
                << " lumaDepthMinus8=" << sps->bit_depth_luma_minus8 << "; "
                << sps_description;
        return RejectH265(reason, details.str());
      }
    } else if (type == H265::kPps) {
      if (!pps_data.empty()) {
        return RejectH265(reason, "duplicate PPS");
      }
      pps_data = nal.subview(2);
    }
  }
  if (!vps || !sps || pps_data.empty() || sps->vps_id != *vps) {
    std::ostringstream details;
    details << "parameter-set links hasVps=" << vps.has_value()
            << " hasSps=" << sps.has_value() << " ppsBytes=" << pps_data.size();
    if (vps) {
      details << " vpsId=" << *vps;
    }
    if (sps) {
      details << " spsVpsId=" << sps->vps_id << " spsId=" << sps->sps_id;
    }
    return RejectH265(reason, details.str());
  }
  const char* vps_violation =
      AmfHevcLevelViolation(vps_level, sps->pic_width_in_luma_samples,
                            sps->pic_height_in_luma_samples,
                            config.framerate_fps, config.bitrate_bps);
  if (vps_violation) {
    std::ostringstream details;
    details << "VPS level violation=" << vps_violation
            << " level=" << static_cast<int>(vps_level)
            << " coded=" << sps->pic_width_in_luma_samples << "x"
            << sps->pic_height_in_luma_samples
            << " fps=" << config.framerate_fps
            << " bitrate=" << config.bitrate_bps;
    return RejectH265(reason, details.str());
  }
  pps = H265PpsParser::ParsePps(pps_data, &*sps);
  if (!pps) {
    const auto rbsp = H265::ParseRbsp(pps_data);
    BitstreamReader reader(rbsp);
    const auto pps_id = reader.ReadExponentialGolomb();
    const auto sps_id = reader.ReadExponentialGolomb();
    const bool dependent = reader.Read<bool>();
    reader.ConsumeBits(1);  // output_flag_present_flag.
    const auto extra = reader.ReadBits(3);
    reader.ConsumeBits(2);  // sign_data_hiding_enabled/cabac_init_present.
    const auto ref_l0 = reader.ReadExponentialGolomb();
    const auto ref_l1 = reader.ReadExponentialGolomb();
    std::ostringstream details;
    details << "PPS common parser rejected; prefixParseOk=" << reader.Ok()
            << " ppsId=" << pps_id << " spsId=" << sps_id
            << " dependent=" << dependent << " extraSliceBits=" << extra
            << " l0RefsMinus1=" << ref_l0 << " l1RefsMinus1=" << ref_l1
            << " expectedSpsId=" << sps->sps_id;
    return RejectH265(reason, details.str());
  }
  // HEVC 7.3.2.3 signals permission for dependent segments in the PPS, not
  // their actual use. 7.3.6.1 only carries dependent_slice_segment_flag when
  // first_slice_segment_in_pic_flag is false. ValidateH265Picture requires
  // exactly one VCL NAL and a first slice, so actual dependent segments remain
  // rejected even when the hardware advertises this PPS permission.
  if (pps->sps_id != sps->sps_id ||
      pps->num_ref_idx_l0_default_active_minus1 != 0 ||
      pps->num_ref_idx_l1_default_active_minus1 != 0) {
    std::ostringstream details;
    details << "PPS parsed contract ppsId=" << pps->pps_id
            << " spsId=" << pps->sps_id << " expectedSpsId=" << sps->sps_id
            << " dependent=" << pps->dependent_slice_segments_enabled_flag
            << " extraSliceBits=" << pps->num_extra_slice_header_bits
            << " l0RefsMinus1=" << pps->num_ref_idx_l0_default_active_minus1
            << " l1RefsMinus1=" << pps->num_ref_idx_l1_default_active_minus1;
    return RejectH265(reason, details.str());
  }
  *state = *pps;
  return true;
}

bool ValidateH265Picture(ArrayView<const uint8_t> bytes,
                         bool idr,
                         const H265PpsParser::PpsState& pps) {
  int slices = 0;
  for (const auto& index : H265::FindNaluIndices(bytes)) {
    const auto nal =
        bytes.subview(index.payload_start_offset, index.payload_size);
    const auto type = H265::ParseNaluType(nal[0]);
    if (type > 31) {
      continue;
    }
    const bool actual_idr = type == H265::kIdrWRadl || type == H265::kIdrNLp;
    if (++slices != 1 || nal.size() <= 2 || actual_idr != idr ||
        (!actual_idr && type != H265::kTrailN && type != H265::kTrailR)) {
      return false;
    }
    const auto rbsp = H265::ParseRbsp(nal.subview(2));
    BitstreamReader reader(rbsp);
    const bool first_slice = reader.Read<bool>();
    // A first slice has no dependent_slice_segment_flag, regardless of the
    // PPS permission bit. The acceptance gate below rejects every non-first
    // slice; do not infer actual dependent use from the PPS permission.
    if (actual_idr) {
      reader.ConsumeBits(1);  // no_output_of_prior_pics_flag.
    }
    const auto pps_id = reader.ReadExponentialGolomb();
    reader.ConsumeBits(pps.num_extra_slice_header_bits);
    const auto slice_type = reader.ReadExponentialGolomb();
    if (!reader.Ok() || !first_slice || pps_id != pps.pps_id ||
        (slice_type != H265::kP && slice_type != H265::kI) ||
        (idr && slice_type != H265::kI)) {
      return false;
    }
  }
  return slices == 1;
}
#endif

AmfH264Capability DescribeAdapter(uint32_t index,
                                  const DXGI_ADAPTER_DESC& desc,
                                  Codec codec) {
  AmfH264Capability capability;
  capability.codec = codec;
  capability.sdk_version = AMF_FULL_VERSION;
  capability.adapter_index = index;
  capability.vendor_id = desc.VendorId;
  capability.device_id = desc.DeviceId;
  capability.dedicated_video_memory_bytes = desc.DedicatedVideoMemory;
  capability.adapter_name = Utf8(desc.Description);
  return capability;
}

// All AMF objects are retired while their runtime's loader reference is held.
// No singleton cleanup is performed that could affect another live encoder.
class AmfSession {
 public:
  ~AmfSession() { Close(); }
  amf::AMFComponent* encoder() { return encoder_; }
  amf::AMFContext* context() { return context_; }
  AMF_RESULT TerminateEncoder() {
    if (encoder_ && !encoder_terminated_) {
      encoder_termination_result_ = encoder_->Terminate();
      encoder_terminated_ = true;
    }
    return encoder_termination_result_;
  }

  Status Open(IDXGIAdapter1* adapter, AmfH264Capability* capability) {
    Close();
    module_ =
        LoadLibraryExW(L"amfrt64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) {
      return Fail(capability, Status::kUnavailable,
                  "System32 amfrt64.dll unavailable, Win32=" +
                      std::to_string(GetLastError()));
    }
    wchar_t module_path[32768] = {};
    if (GetModuleFileNameW(module_, module_path,
                           static_cast<DWORD>(std::size(module_path)))) {
      capability->runtime_path = Utf8(module_path);
    }
    const auto init = reinterpret_cast<AMFInit_Fn>(
        GetProcAddress(module_, AMF_INIT_FUNCTION_NAME));
    const auto version = reinterpret_cast<AMFQueryVersion_Fn>(
        GetProcAddress(module_, AMF_QUERY_VERSION_FUNCTION_NAME));
    if (!init || !version) {
      return Fail(capability, Status::kUnavailable,
                  "AMFInit/AMFQueryVersion export unavailable");
    }
    amf_uint64 runtime_version = 0;
    AMF_RESULT result = version(&runtime_version);
    capability->runtime_version = runtime_version;
    if (result != AMF_OK) {
      return Fail(capability, Status::kUnavailable,
                  AmfFailure("AMFQueryVersion", result));
    }
    result = init(AMF_FULL_VERSION, &factory_);
    if (result != AMF_OK || !factory_) {
      return Fail(capability, Status::kUnavailable,
                  AmfFailure("AMFInit", result));
    }
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1,
                                           D3D_FEATURE_LEVEL_11_0};
    const HRESULT created = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        requested, static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
        &device_, &level, &device_context_);
    if (FAILED(created)) {
      return Fail(capability, Status::kUnavailable,
                  "AMD D3D11 hardware device creation failed, HRESULT=" +
                      std::to_string(static_cast<uint32_t>(created)));
    }
    result = factory_->CreateContext(&context_);
    if (result != AMF_OK || !context_) {
      return Fail(capability, Status::kUnavailable,
                  AmfFailure("CreateContext", result));
    }
    result = context_->InitDX11(device_.Get(), amf::AMF_DX11_1);
    if (result != AMF_OK) {
      return Fail(capability, Status::kUnavailable,
                  AmfFailure("InitDX11", result));
    }
    const wchar_t* component = AMFVideoEncoderVCE_AVC;
#if defined(RTC_ENABLE_H265)
    if (capability->codec == Codec::kH265) {
      component = AMFVideoEncoder_HEVC;
    }
#endif
    result = factory_->CreateComponent(context_, component, &encoder_);
    encoder_terminated_ = false;
    if (result != AMF_OK || !encoder_) {
      return Fail(
          capability, Status::kUnavailable,
          AmfFailure(capability->codec == Codec::kH264 ? "CreateComponent AVC"
                                                       : "CreateComponent HEVC",
                     result));
    }
    return ReadCapabilities(capability);
  }

  // Backend input observers are verified after all SDK references retire.
  // Keep the loader reference until that check, so a late callback cannot
  // return into an unloaded AMF image.
  Status Close(std::string* reason = nullptr, bool unload_runtime = true) {
    Status status = Status::kOk;
    std::string failure;
    if (encoder_) {
      const AMF_RESULT result = TerminateEncoder();
      if (result != AMF_OK) {
        status = Status::kError;
        failure = AmfFailure("encoder Terminate", result);
      }
      encoder_.Release();
    }
    if (context_) {
      const AMF_RESULT result = context_->Terminate();
      if (result != AMF_OK) {
        status = Status::kError;
        if (!failure.empty()) {
          failure += "; ";
        }
        failure += AmfFailure("context Terminate", result);
      }
      context_.Release();
    }
    device_context_.Reset();
    device_.Reset();
    factory_ = nullptr;
    encoder_terminated_ = false;
    encoder_termination_result_ = AMF_OK;
    if (unload_runtime) {
      ReleaseRuntime(status == Status::kOk);
    }
    if (reason) {
      *reason = std::move(failure);
    }
    return status;
  }

  void ReleaseRuntime(bool safe_to_unload) {
    if (!module_) {
      return;
    }
    if (safe_to_unload) {
      FreeLibrary(module_);
    } else {
      // The driver violated normal retirement. Retain this loader reference
      // until process exit instead of unloading code still used by the SDK.
      // This is an error, never a successful resource-release acceptance.
      RTC_LOG(LS_ERROR) << "AMF_CLOSE runtime loader reference retained until "
                           "process exit after failed SDK retirement";
    }
    module_ = nullptr;
  }

 private:
  Status ReadCapabilities(AmfH264Capability* capability) {
    amf::AMFCapsPtr caps;
    AMF_RESULT result = encoder_->GetCaps(&caps);
    if (result != AMF_OK || !caps) {
      return Fail(capability, Status::kUnsupported,
                  AmfFailure("GetCaps hardware encoder", result));
    }
    capability->acceleration_type = caps->GetAccelerationType();
    capability->hardware_accelerated =
        caps->GetAccelerationType() == amf::AMF_ACCEL_HARDWARE;
    if (!capability->hardware_accelerated) {
      return Fail(capability, Status::kUnsupported,
                  "AMF component does not report hardware acceleration");
    }
    amf::AMFIOCapsPtr input;
    result = caps->GetInputCaps(&input);
    if (result != AMF_OK || !input) {
      return Fail(capability, Status::kUnsupported,
                  AmfFailure("GetInputCaps hardware encoder", result));
    }
    input->GetWidthRange(&capability->min_width, &capability->max_width);
    input->GetHeightRange(&capability->min_height, &capability->max_height);
    capability->vertical_alignment = input->GetVertAlign();
    for (amf_int32 index = 0; index < input->GetNumOfFormats(); ++index) {
      amf::AMF_SURFACE_FORMAT format = amf::AMF_SURFACE_UNKNOWN;
      amf_bool native = false;
      if (input->GetFormatAt(index, &format, &native) == AMF_OK &&
          format == amf::AMF_SURFACE_NV12) {
        capability->nv12_input = true;
      }
    }
    for (amf_int32 index = 0; index < input->GetNumOfMemoryTypes(); ++index) {
      amf::AMF_MEMORY_TYPE memory = amf::AMF_MEMORY_UNKNOWN;
      amf_bool native = false;
      if (input->GetMemoryTypeAt(index, &memory, &native) != AMF_OK) {
        continue;
      }
      capability->host_input |= memory == amf::AMF_MEMORY_HOST;
      capability->dx11_input |= memory == amf::AMF_MEMORY_DX11;
    }
    amf_int64 bitrate = 0;
    amf_int64 level = 0;
    const wchar_t* bitrate_property = AMF_VIDEO_ENCODER_CAP_MAX_BITRATE;
    const wchar_t* level_property = AMF_VIDEO_ENCODER_CAP_MAX_LEVEL;
#if defined(RTC_ENABLE_H265)
    if (capability->codec == Codec::kH265) {
      bitrate_property = AMF_VIDEO_ENCODER_HEVC_CAP_MAX_BITRATE;
      level_property = AMF_VIDEO_ENCODER_HEVC_CAP_MAX_LEVEL;
    }
#endif
    const AMF_RESULT bitrate_result =
        caps->GetProperty(bitrate_property, &bitrate);
    const AMF_RESULT level_result = caps->GetProperty(level_property, &level);
    capability->max_bitrate_query_result = static_cast<int>(bitrate_result);
    capability->max_level_query_result = static_cast<int>(level_result);
    if (bitrate_result == AMF_OK) {
      capability->max_bitrate_bps = bitrate;
    }
    if (level_result == AMF_OK) {
      if (capability->codec == Codec::kH264) {
        capability->max_h264_level = level;
      } else {
        capability->max_h265_level = level;
      }
    }
    if (bitrate_result != AMF_OK || level_result != AMF_OK) {
      return Fail(capability, Status::kUnsupported,
                  "AMF max bitrate/level capabilities unavailable; "
                  "maxBitrateResult=" +
                      std::to_string(capability->max_bitrate_query_result) +
                      "; maxLevelResult=" +
                      std::to_string(capability->max_level_query_result));
    }
#if defined(RTC_ENABLE_H265)
    if (capability->codec == Codec::kH265) {
      // Some real AMF hardware components omit these optional maximum caps.
      // Query both independently and preserve missing maxima as unknown; the
      // encoder's property validation is the gate for our exact requested
      // Main/Main-tier configuration, not a fabricated maximum capability.
      amf_int64 profile = -1;
      amf_int64 tier = -1;
      const AMF_RESULT profile_result =
          caps->GetProperty(AMF_VIDEO_ENCODER_HEVC_CAP_MAX_PROFILE, &profile);
      const AMF_RESULT tier_result =
          caps->GetProperty(AMF_VIDEO_ENCODER_HEVC_CAP_MAX_TIER, &tier);
      capability->max_profile_query_result = static_cast<int>(profile_result);
      capability->max_tier_query_result = static_cast<int>(tier_result);
      capability->max_profile_source =
          profile_result == AMF_OK ? "GetCaps" : "UNKNOWN";
      capability->max_tier_source =
          tier_result == AMF_OK ? "GetCaps" : "UNKNOWN";
      if (profile_result == AMF_OK) {
        capability->max_profile = profile;
      }
      if (tier_result == AMF_OK) {
        capability->max_tier = tier;
      }
      amf::AMFVariant validated_profile;
      amf::AMFVariant validated_tier;
      const AMF_RESULT profile_validation = encoder_->ValidateProperty(
          AMF_VIDEO_ENCODER_HEVC_PROFILE,
          amf::AMFVariant(
              static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN)),
          &validated_profile);
      const AMF_RESULT tier_validation = encoder_->ValidateProperty(
          AMF_VIDEO_ENCODER_HEVC_TIER,
          amf::AMFVariant(
              static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_TIER_MAIN)),
          &validated_tier);
      capability->main_profile_validation_result =
          static_cast<int>(profile_validation);
      capability->main_tier_validation_result =
          static_cast<int>(tier_validation);
      capability->main_profile_configuration_source = "ValidateProperty";
      capability->main_tier_configuration_source = "ValidateProperty";
      if (validated_profile.type == amf::AMF_VARIANT_INT64) {
        capability->main_profile_validated_value = validated_profile.int64Value;
      }
      if (validated_tier.type == amf::AMF_VARIANT_INT64) {
        capability->main_tier_validated_value = validated_tier.int64Value;
      }
      capability->main_profile_configurable =
          profile_validation == AMF_OK &&
          capability->main_profile_validated_value ==
              AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN;
      capability->main_tier_configurable =
          tier_validation == AMF_OK && capability->main_tier_validated_value ==
                                           AMF_VIDEO_ENCODER_HEVC_TIER_MAIN;
      RTC_LOG(LS_INFO) << "AMF_HEVC_CONFIG_CAPS maxProfileResult="
                       << capability->max_profile_query_result
                       << " maxProfile=" << capability->max_profile
                       << " maxTierResult=" << capability->max_tier_query_result
                       << " maxTier=" << capability->max_tier
                       << " mainProfileValidation="
                       << capability->main_profile_validation_result
                       << " mainProfileValidated="
                       << capability->main_profile_validated_value
                       << " mainTierValidation="
                       << capability->main_tier_validation_result
                       << " mainTierValidated="
                       << capability->main_tier_validated_value;
      if (!capability->main_profile_configurable ||
          !capability->main_tier_configurable ||
          (profile_result == AMF_OK &&
           profile < AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN) ||
          (tier_result == AMF_OK && tier < AMF_VIDEO_ENCODER_HEVC_TIER_MAIN)) {
        return Fail(
            capability, Status::kUnsupported,
            "AMF HEVC Main/Main-tier configuration rejected; "
            "maxProfileResult=" +
                std::to_string(capability->max_profile_query_result) +
                "; maxTierResult=" +
                std::to_string(capability->max_tier_query_result) +
                "; mainProfileValidation=" +
                std::to_string(capability->main_profile_validation_result) +
                "; mainProfileValidated=" +
                std::to_string(capability->main_profile_validated_value) +
                "; mainTierValidation=" +
                std::to_string(capability->main_tier_validation_result) +
                "; mainTierValidated=" +
                std::to_string(capability->main_tier_validated_value));
      }
    }
#endif
    if (!capability->nv12_input || !capability->host_input ||
        !capability->dx11_input) {
      return Fail(capability, Status::kUnsupported,
                  "AMF requires NV12, HOST upload and DX11 input support");
    }
    capability->reason.clear();
    capability->status = Status::kOk;
    return Status::kOk;
  }

  static Status Fail(AmfH264Capability* capability,
                     Status status,
                     std::string reason) {
    capability->status = status;
    capability->reason = std::move(reason);
    return status;
  }

  HMODULE module_ = nullptr;
  amf::AMFFactory* factory_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> device_context_;
  amf::AMFContextPtr context_;
  amf::AMFComponentPtr encoder_;
  bool encoder_terminated_ = false;
  AMF_RESULT encoder_termination_result_ = AMF_OK;
};

// Release notifications only update an atomic availability flag. The SDK may
// retain a host surface after producing output, so never overwrite it merely
// because QueryOutput succeeded. Slot storage and observers outlive Terminate.
struct InputSlot final : amf::AMFSurfaceObserver {
  void AMF_STD_CALL OnSurfaceDataRelease(amf::AMFSurface*) override {
    available.store(true, std::memory_order_release);
  }
  std::atomic<bool> available{true};
  std::vector<uint8_t> pixels;
};

class AmfBackend final : public VideoEncoderBackend {
 public:
  explicit AmfBackend(Codec codec) : codec_(codec) {}
  ~AmfBackend() override {
    Close();
    if (slots_ && !InputsRetired()) {
      // A failed SDK retirement may still call an observer after this backend
      // dies. Preserve this instance's fixed four-slot allocation; never add
      // a global retirement queue or claim these resources were released.
      (void)slots_.release();
      RTC_LOG(LS_ERROR) << "AMF_CLOSE four owned input slots retained until "
                           "process exit because observers did not retire";
    }
  }

  Info GetInfo() const override {
    return {codec_ == Codec::kH264 ? "AMF H264 / D3D11" : "AMF H265 / D3D11",
            true};
  }
  std::string_view LastError() const override { return error_; }
  Status LastCloseStatus() const override { return last_close_status_; }

  Status Open(const Config& config) override {
    Close();
    std::string retirement_reason;
    if (HardwareEncoderProbes().Check(HardwareEncoderVendor::kAmf,
                                      &retirement_reason) ==
        Status::kRetirementFailed) {
      return Fail(Status::kRetirementFailed, retirement_reason);
    }
    if (retirement_failed_) {
      return Fail(Status::kError,
                  "AMF cannot reopen after failed resource retirement: " +
                      close_error_);
    }
    error_.clear();
    if (!ValidConfig(config, codec_)) {
      return Fail(Status::kInvalidArgument,
                  "invalid AMF codec/level/NV12/rate configuration");
    }
    if (codec_ == Codec::kH265) {
      // Visible dimensions are a lower bound: impossible requests can be
      // rejected without a driver call. Actual coded padding is validated
      // independently against both emitted VPS and SPS levels after Init.
      const char* violation =
          AmfHevcLevelViolation(config.h265_level, config.width, config.height,
                                config.framerate_fps, config.bitrate_bps);
      if (violation) {
        return Fail(Status::kUnsupported, violation);
      }
    }
    config_ = config;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return Fail(Status::kUnavailable, "DXGI factory unavailable");
    }
    Status last_status = Status::kUnavailable;
    bool found_amd = false;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
      if (enumerated == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(enumerated)) {
        return Abort(Status::kError, "DXGI adapter enumeration failed");
      }
      DXGI_ADAPTER_DESC desc = {};
      if (FAILED(adapter->GetDesc(&desc)) || desc.VendorId != kAmdVendor) {
        continue;
      }
      found_amd = true;
      capability_ = DescribeAdapter(index, desc, codec_);
      last_status = session_.Open(adapter.Get(), &capability_);
      if (last_status == Status::kOk) {
        last_status = InitializeEncoder();
      } else {
        error_ = capability_.reason;
      }
      if (last_status == Status::kOk) {
        initialized_ = true;
        need_idr_ = true;
        error_.clear();
        return Status::kOk;
      }
      const std::string operation_error = error_;
      Close();
      if (retirement_failed_) {
        return Fail(Status::kError, operation_error + "; " + close_error_);
      }
      error_ = operation_error;
    }
    if (!found_amd) {
      return Fail(Status::kUnavailable, "no AMD DXGI graphics adapter");
    }
    return last_status;
  }

  Status SetRates(const Rates& rates) override {
    if (!initialized_) {
      return Fail(Status::kError, "AMF encoder not initialized");
    }
    if (!ValidRates(rates)) {
      return Fail(Status::kInvalidArgument, "invalid AMF bitrate/framerate");
    }
    if (rates.bitrate_bps == config_.bitrate_bps &&
        rates.framerate_fps == config_.framerate_fps) {
      return Status::kOk;
    }
    Config config = config_;
    config.bitrate_bps = rates.bitrate_bps;
    config.framerate_fps = rates.framerate_fps;
    if (rates.framerate_fps == config_.framerate_fps) {
      return UpdateBitrate(config);
    }
    // Each successful Encode already returned its corresponding AU. FPS
    // changes still reopen safely and require IDR in the new session.
    RTC_LOG(LS_INFO) << "AMF_RECONFIG codec="
                     << (codec_ == Codec::kH264 ? "H264" : "H265")
                     << " action=close_reinitialize bitrate="
                     << rates.bitrate_bps << " fps=" << rates.framerate_fps;
    return Open(config);
  }

  Status Encode(const Frame& frame,
                bool force_key_frame,
                Packet* packet) override {
    if (packet) {
      *packet = {};
    }
    if (!initialized_) {
      return Fail(Status::kError, "AMF encoder not initialized");
    }
    if (!packet || !frame.y || !frame.uv || frame.width != config_.width ||
        frame.height != config_.height || frame.stride_y < frame.width ||
        frame.stride_uv < frame.width ||
        frame.id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        next_pts_ > std::numeric_limits<int64_t>::max() -
                        AMF_SECOND / config_.framerate_fps) {
      return Fail(Status::kInvalidArgument, "invalid AMF input frame");
    }
    AMF_RESULT result = session_.encoder()->QueryOutput(&output_);
    if (output_ || (result != AMF_OK && result != AMF_REPEAT)) {
      return Abort(Status::kError, "AMF returned an unsubmitted/stale output");
    }
    const auto deadline = Clock::now() + kPollTimeout;
    InputSlot* slot = nullptr;
    while (!slot) {
      for (auto& candidate : *slots_) {
        if (candidate.available.load(std::memory_order_acquire)) {
          slot = &candidate;
          break;
        }
      }
      if (slot) {
        break;
      }
      if (Clock::now() >= deadline) {
        return Abort(Status::kTimeout, "AMF input surfaces not retired");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const size_t luma_bytes = static_cast<size_t>(stride_) * frame.height;
    for (int row = 0; row < frame.height; ++row) {
      std::memcpy(slot->pixels.data() + static_cast<size_t>(row) * stride_,
                  frame.y + static_cast<size_t>(row) * frame.stride_y,
                  frame.width);
    }
    for (int row = 0; row < frame.height / 2; ++row) {
      std::memcpy(
          slot->pixels.data() + luma_bytes + static_cast<size_t>(row) * stride_,
          frame.uv + static_cast<size_t>(row) * frame.stride_uv, frame.width);
    }
    slot->available.store(false, std::memory_order_release);
    result = session_.context()->CreateSurfaceFromHostNative(
        amf::AMF_SURFACE_NV12, frame.width, frame.height, stride_, 0,
        slot->pixels.data(), &input_, slot);
    if (result != AMF_OK || !input_) {
      if (!input_) {
        // No surface owns these planes and therefore no observer will fire.
        slot->available.store(true, std::memory_order_release);
      }
      return Abort(Status::kError,
                   AmfFailure("CreateSurfaceFromHostNative", result));
    }
    const bool request_idr = force_key_frame || need_idr_;
    input_->SetPts(next_pts_);
    input_->SetDuration(AMF_SECOND / config_.framerate_fps);
    if (input_->SetProperty(kFrameIdProperty,
                            static_cast<amf_int64>(frame.id)) != AMF_OK ||
        !SetInputPicture(request_idr)) {
      return Abort(Status::kError, "AMF per-picture IDR/identity setup failed");
    }
    // HOST is explicitly reported by the hardware component's input caps;
    // AMF uploads it through this AMD D3D11 context. No software component or
    // CPU encoder is selected. The caller's borrowed planes were copied above.
    do {
      result = session_.encoder()->SubmitInput(input_);
      if (result == AMF_OK || result == AMF_NEED_MORE_INPUT) {
        break;
      }
      if (result != AMF_INPUT_FULL) {
        return Abort(Status::kError, AmfFailure("SubmitInput", result));
      }
      if (Clock::now() >= deadline) {
        return Abort(Status::kTimeout, "AMF SubmitInput timed out");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
    while (!output_) {
      result = session_.encoder()->QueryOutput(&output_);
      if (result != AMF_OK && result != AMF_REPEAT) {
        return Abort(Status::kError, AmfFailure("QueryOutput", result));
      }
      if (output_) {
        break;
      }
      if (Clock::now() >= deadline) {
        // Never feed another frame or flush to manufacture synchronous output.
        return Abort(Status::kTimeout,
                     "AMF did not complete this frame within polling window");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    amf_int64 output_id = -1;
    amf_int64 output_type = -1;
    const wchar_t* output_type_property = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE;
    amf_int64 type_idr = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
    amf_int64 type_i = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
    amf_int64 type_p = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P;
#if defined(RTC_ENABLE_H265)
    if (codec_ == Codec::kH265) {
      output_type_property = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE;
      type_idr = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
      type_i = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I;
      type_p = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_P;
    }
#endif
    if (output_->GetPts() != next_pts_ ||
        output_->GetProperty(kFrameIdProperty, &output_id) != AMF_OK ||
        output_id != static_cast<amf_int64>(frame.id) ||
        output_->GetProperty(output_type_property, &output_type) != AMF_OK ||
        (output_type != type_idr && output_type != type_i &&
         output_type != type_p)) {
      return Abort(Status::kError, "AMF output PTS/frame ID/type mismatch");
    }
    buffer_ = amf::AMFBufferPtr(output_);
    if (!buffer_ || buffer_->GetMemoryType() != amf::AMF_MEMORY_HOST ||
        buffer_->GetSize() == 0 || buffer_->GetSize() > kMaximumPacketBytes ||
        !buffer_->GetNative()) {
      return Abort(Status::kError, "AMF expected one owned output buffer");
    }
    const ArrayView<const uint8_t> bytes(
        static_cast<const uint8_t*>(buffer_->GetNative()), buffer_->GetSize());
    const bool idr = output_type == type_idr;
    std::string picture_error;
    bool picture_valid = codec_ == Codec::kH264 && AnnexB(bytes) &&
                         ValidatePicture(bytes, idr, pps_id_);
    bool has_headers = false;
#if defined(RTC_ENABLE_H265)
    if (codec_ == Codec::kH265) {
      picture_valid = H265AnnexB(bytes, &picture_error) &&
                      ValidateH265Picture(bytes, idr, h265_pps_);
      const bool has_vps = HasH265Nal(bytes, H265::kVps);
      const bool has_sps = HasH265Nal(bytes, H265::kSps);
      const bool has_pps = HasH265Nal(bytes, H265::kPps);
      has_headers = has_vps && has_sps && has_pps;
      if (has_vps || has_sps || has_pps) {
        H265PpsParser::PpsState emitted;
        std::string header_error;
        if (!ValidateH265Headers(bytes, config_, &emitted, &header_error)) {
          return Abort(
              Status::kError,
              "AMF emitted incompatible HEVC parameter sets: " + header_error);
        }
        if (emitted.pps_id != h265_pps_.pps_id ||
            emitted.sps_id != h265_pps_.sps_id ||
            emitted.num_extra_slice_header_bits !=
                h265_pps_.num_extra_slice_header_bits) {
          std::ostringstream details;
          details << "AMF changed HEVC parameter sets ppsId=" << emitted.pps_id
                  << " expectedPpsId=" << h265_pps_.pps_id
                  << " spsId=" << emitted.sps_id
                  << " expectedSpsId=" << h265_pps_.sps_id
                  << " extraSliceBits=" << emitted.num_extra_slice_header_bits
                  << " expectedExtraSliceBits="
                  << h265_pps_.num_extra_slice_header_bits;
          return Abort(Status::kError, details.str());
        }
      }
    }
#endif
    if (!picture_valid || (request_idr && !idr)) {
      return Abort(Status::kError,
                   "AMF emitted invalid picture/requested IDR" +
                       (picture_error.empty() ? "" : ": " + picture_error));
    }
    if (codec_ == Codec::kH264) {
      const bool has_sps = HasNal(bytes, H264::kSps);
      const bool has_pps = HasNal(bytes, H264::kPps);
      has_headers = has_sps && has_pps;
      if (has_sps || has_pps) {
        uint32_t emitted_pps = 0;
        if (!ValidateHeaders(bytes, config_, &emitted_pps) ||
            emitted_pps != pps_id_) {
          return Abort(Status::kError,
                       "AMF emitted incompatible parameter sets");
        }
      }
    }
    std::vector<uint8_t> owned;
    if (idr && !has_headers) {
      if (headers_.size() > kMaximumPacketBytes - bytes.size()) {
        return Abort(Status::kError, "AMF access unit exceeds byte limit");
      }
      owned = headers_;
    }
    owned.insert(owned.end(), bytes.begin(), bytes.end());
    buffer_.Release();
    output_.Release();
    input_.Release();
    // Frame output mode must not produce a second AU for one submission.
    result = session_.encoder()->QueryOutput(&output_);
    if (output_ || (result != AMF_OK && result != AMF_REPEAT)) {
      return Abort(Status::kError, "AMF returned extra output for one input");
    }
    packet->bytes = std::move(owned);
    packet->frame_id = frame.id;
    packet->key_frame = idr;
    next_pts_ += AMF_SECOND / config_.framerate_fps;
    need_idr_ = false;
    error_.clear();
    return Status::kOk;
  }

  void Close() override {
    initialized_ = false;
    // Terminate before retiring submitted HOST storage or observer objects.
    // Release our AMF refs before session_ unloads the driver's runtime.
    session_.TerminateEncoder();
    buffer_.Release();
    output_.Release();
    input_.Release();
    std::string failure;
    const Status result = session_.Close(&failure, false);
    const bool inputs_retired = InputsRetired();
    if (!inputs_retired) {
      if (!failure.empty()) {
        failure += "; ";
      }
      failure += "AMF input surface observers did not retire after SDK release";
    }
    if (result != Status::kOk || !inputs_retired) {
      retirement_failed_ = true;
      if (close_error_.empty()) {
        close_error_ = std::move(failure);
      }
    }
    last_close_status_ = retirement_failed_ ? Status::kError : Status::kOk;
    session_.ReleaseRuntime(!retirement_failed_);
    if (inputs_retired && slots_) {
      for (auto& slot : *slots_) {
        slot.pixels.clear();
      }
    }
    if (retirement_failed_) {
      error_ = close_error_;
      HardwareEncoderProbes().RecordRetirementFailure(
          HardwareEncoderVendor::kAmf, close_error_);
      RTC_LOG(LS_ERROR) << "AMF_CLOSE failed: " << close_error_;
    }
    headers_.clear();
    pps_id_ = 0;
#if defined(RTC_ENABLE_H265)
    h265_pps_ = {};
#endif
    next_pts_ = 0;
    initial_vbv_bits_ = 0;
    need_idr_ = true;
  }

 private:
  bool InputsRetired() const {
    if (!slots_) {
      return true;
    }
    for (const auto& slot : *slots_) {
      if (!slot.available.load(std::memory_order_acquire)) {
        return false;
      }
    }
    return true;
  }
  bool SetInputPicture(bool idr) {
#if defined(RTC_ENABLE_H265)
    if (codec_ == Codec::kH265) {
      return input_->SetProperty(
                 AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                 static_cast<amf_int64>(
                     idr ? AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR
                         : AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_P)) == AMF_OK &&
             input_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, idr) ==
                 AMF_OK;
    }
#endif
    return input_->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                               static_cast<amf_int64>(
                                   idr ? AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR
                                       : AMF_VIDEO_ENCODER_PICTURE_TYPE_P)) ==
               AMF_OK &&
           input_->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, idr) == AMF_OK &&
           input_->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, idr) == AMF_OK;
  }
  template <typename T>
  bool Set(const wchar_t* property, const T& value) {
    const auto result = session_.encoder()->SetProperty(property, value);
    if (result == AMF_OK) {
      return true;
    }
    error_ = "AMF SetProperty " + Utf8(property) + ": " +
             std::to_string(static_cast<int>(result));
    return false;
  }

  template <typename T>
  bool Check(const wchar_t* property, const T& expected) {
    T actual{};
    const auto result = session_.encoder()->GetProperty(property, &actual);
    if (result == AMF_OK && actual == expected) {
      return true;
    }
    error_ = "AMF property readback mismatch: " + Utf8(property);
    return false;
  }

  bool ReadIntegerExactly(const wchar_t* property,
                          amf_int64 expected,
                          amf_int64* value = nullptr) {
    amf::AMFVariant actual;
    const AMF_RESULT result = session_.encoder()->GetProperty(
        property, static_cast<amf::AMFVariantStruct*>(&actual));
    if (result == AMF_OK && actual.type == amf::AMF_VARIANT_INT64 &&
        actual.int64Value == expected) {
      if (value) {
        *value = actual.int64Value;
      }
      return true;
    }
    std::ostringstream details;
    details << "AMF exact integer readback " << Utf8(property)
            << " result=" << static_cast<int>(result)
            << " type=" << static_cast<int>(actual.type)
            << " expected=" << expected;
    if (actual.type == amf::AMF_VARIANT_INT64) {
      details << " actual=" << actual.int64Value;
    }
    error_ = details.str();
    return false;
  }

  Status ValidateDynamicBitrateProperty(const wchar_t* property,
                                        amf_int64 bitrate,
                                        std::string* reason) {
    const amf::AMFPropertyInfo* info = nullptr;
    const AMF_RESULT info_result =
        session_.encoder()->GetPropertyInfo(property, &info);
    if (info_result != AMF_OK || !info) {
      *reason = "AMF dynamic property metadata " + Utf8(property) +
                " result=" + std::to_string(static_cast<int>(info_result));
      return info_result == AMF_NOT_FOUND || info_result == AMF_NOT_SUPPORTED ||
                     info_result == AMF_NOT_IMPLEMENTED
                 ? Status::kUnsupported
                 : Status::kError;
    }
    if (info->type != amf::AMF_VARIANT_INT64 || !info->AllowedRead() ||
        !info->AllowedWrite() || !info->AllowedChangeInRuntime()) {
      *reason = "AMF dynamic property access " + Utf8(property) +
                " type=" + std::to_string(static_cast<int>(info->type)) +
                " access=" + std::to_string(static_cast<int>(info->accessType));
      return Status::kUnsupported;
    }
    amf::AMFVariant validated;
    const AMF_RESULT result = session_.encoder()->ValidateProperty(
        property, amf::AMFVariant(bitrate), &validated);
    if (result == AMF_OK && validated.type == amf::AMF_VARIANT_INT64 &&
        validated.int64Value == bitrate) {
      return Status::kOk;
    }
    std::ostringstream details;
    details << "AMF dynamic property validation " << Utf8(property)
            << " result=" << static_cast<int>(result)
            << " type=" << static_cast<int>(validated.type)
            << " requested=" << bitrate;
    if (validated.type == amf::AMF_VARIANT_INT64) {
      details << " validated=" << validated.int64Value;
    }
    *reason = details.str();
    return result == AMF_NOT_FOUND || result == AMF_NOT_SUPPORTED ||
                   result == AMF_NOT_IMPLEMENTED
               ? Status::kUnsupported
               : Status::kError;
  }

  Status UpdateBitrate(const Config& config) {
    if (config.bitrate_bps > capability_.max_bitrate_bps) {
      return Fail(Status::kUnsupported,
                  "AMF bitrate exceeds reported hardware capability");
    }
#if defined(RTC_ENABLE_H265)
    if (codec_ == Codec::kH265) {
      // Validate the actual VPS/SPS levels and coded padding against the new
      // rate; a requested level alone cannot prove the emitted level allows it.
      H265PpsParser::PpsState pps;
      std::string reason;
      if (!ValidateH265Headers(headers_, config, &pps, &reason)) {
        return Fail(
            Status::kUnsupported,
            "AMF HEVC bitrate violates current parameter sets: " + reason);
      }
    }
#endif
    if (initial_vbv_bits_ <= 0) {
      return Abort(Status::kError, "AMF initial VBV capacity unavailable");
    }
    const wchar_t* target_property = AMF_VIDEO_ENCODER_TARGET_BITRATE;
    const wchar_t* peak_property = AMF_VIDEO_ENCODER_PEAK_BITRATE;
    const wchar_t* vbv_property = AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE;
#if defined(RTC_ENABLE_H265)
    if (codec_ == Codec::kH265) {
      target_property = AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE;
      peak_property = AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE;
      vbv_property = AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE;
    }
#endif
    const amf_int64 bitrate = config.bitrate_bps;
    std::string target_reason;
    std::string peak_reason;
    // Complete both preflights before any mutation. Only explicitly missing
    // runtime support may use the old, logged Close/Open configuration path.
    const Status target_status = ValidateDynamicBitrateProperty(
        target_property, bitrate, &target_reason);
    const Status peak_status =
        ValidateDynamicBitrateProperty(peak_property, bitrate, &peak_reason);
    if (target_status != Status::kOk || peak_status != Status::kOk) {
      const std::string reason = target_reason + "; " + peak_reason;
      if (target_status == Status::kError || peak_status == Status::kError) {
        return Abort(Status::kError, reason);
      }
      RTC_LOG(LS_INFO) << "AMF_RECONFIG codec="
                       << (codec_ == Codec::kH264 ? "H264" : "H265")
                       << " action=close_reinitialize bitrate=" << bitrate
                       << " fps=" << config.framerate_fps
                       << " reason=dynamic_property_unsupported details="
                       << reason;
      return Open(config);
    }
    const uint32_t old_bitrate = config_.bitrate_bps;
    // Preserve target <= peak during a CBR update. No other frame is pending,
    // and the adapter serializes all calls on its encoder sequence.
    const wchar_t* first =
        bitrate > old_bitrate ? peak_property : target_property;
    const wchar_t* second =
        bitrate > old_bitrate ? target_property : peak_property;
    if (!Set(first, bitrate) || !Set(second, bitrate)) {
      return Abort(Status::kError, error_);
    }
    // VBV is a capacity in bits, not a bitrate ceiling. In particular HEVC
    // declares it static; retain the initial capacity instead of changing it
    // in place or silently recreating a session for tiny bitrate adjustments.
    const bool target_read = ReadIntegerExactly(target_property, bitrate);
    const std::string target_error = target_read ? "" : error_;
    const bool peak_read = ReadIntegerExactly(peak_property, bitrate);
    const std::string peak_error = peak_read ? "" : error_;
    const bool vbv_read = ReadIntegerExactly(vbv_property, initial_vbv_bits_);
    const std::string vbv_error = vbv_read ? "" : error_;
    if (!target_read || !peak_read || !vbv_read) {
      // A partial update cannot be rolled back by assuming further setters
      // succeed. Retire the session and let the existing failure policy run.
      return Abort(Status::kError,
                   target_error + "; " + peak_error + "; " + vbv_error);
    }
    config_.bitrate_bps = config.bitrate_bps;
    error_.clear();
    RTC_LOG(LS_INFO) << "AMF_RECONFIG codec="
                     << (codec_ == Codec::kH264 ? "H264" : "H265")
                     << " action=update_bitrate old_bitrate=" << old_bitrate
                     << " bitrate=" << bitrate
                     << " fps=" << config_.framerate_fps
                     << " vbv_bits=" << initial_vbv_bits_;
    // Successful bitrate-only updates retain PTS, GOP state and need_idr_.
    return Status::kOk;
  }

  Status InitializeEncoder() {
#if defined(RTC_ENABLE_H265)
    if (codec_ == Codec::kH265) {
      return InitializeH265Encoder();
    }
#endif
    if (config_.width < capability_.min_width ||
        config_.width > capability_.max_width ||
        config_.height < capability_.min_height ||
        config_.height > capability_.max_height ||
        config_.h264_level > capability_.max_h264_level ||
        config_.bitrate_bps > capability_.max_bitrate_bps) {
      return Fail(Status::kUnsupported,
                  "AMF request exceeds reported hardware capabilities");
    }
    if (!Set(AMF_VIDEO_ENCODER_USAGE,
             static_cast<amf_int64>(
                 AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY)) ||
        !Set(AMF_VIDEO_ENCODER_QUALITY_PRESET,
             static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED)) ||
        !Set(AMF_VIDEO_ENCODER_FRAMESIZE,
             AMFConstructSize(config_.width, config_.height)) ||
        !Set(AMF_VIDEO_ENCODER_PROFILE,
             static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_BASELINE)) ||
        !Set(AMF_VIDEO_ENCODER_PROFILE_LEVEL,
             static_cast<amf_int64>(config_.h264_level)) ||
        !Set(AMF_VIDEO_ENCODER_SCANTYPE,
             static_cast<amf_int64>(AMF_VIDEO_ENCODER_SCANTYPE_PROGRESSIVE)) ||
        !Set(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true) ||
        !Set(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, false) ||
        !Set(AMF_VIDEO_ENCODER_PREENCODE_ENABLE,
             static_cast<amf_int64>(AMF_VIDEO_ENCODER_PREENCODE_DISABLED)) ||
        !Set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
             static_cast<amf_int64>(
                 AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)) ||
        !Set(AMF_VIDEO_ENCODER_FRAMERATE,
             AMFConstructRate(config_.framerate_fps, 1)) ||
        !Set(AMF_VIDEO_ENCODER_TARGET_BITRATE,
             static_cast<amf_int64>(config_.bitrate_bps)) ||
        !Set(AMF_VIDEO_ENCODER_PEAK_BITRATE,
             static_cast<amf_int64>(config_.bitrate_bps)) ||
        !Set(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
             static_cast<amf_int64>(config_.bitrate_bps)) ||
        !Set(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, false) ||
        !Set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0)) ||
        !Set(AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES,
             static_cast<amf_int64>(0)) ||
        !Set(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, static_cast<amf_int64>(1)) ||
        !Set(AMF_VIDEO_ENCODER_MAX_NUM_TEMPORAL_LAYERS,
             static_cast<amf_int64>(1)) ||
        !Set(AMF_VIDEO_ENCODER_SLICES_PER_FRAME, static_cast<amf_int64>(1)) ||
        !Set(AMF_VIDEO_ENCODER_OUTPUT_MODE,
             static_cast<amf_int64>(AMF_VIDEO_ENCODER_OUTPUT_MODE_FRAME)) ||
        !Set(AMF_VIDEO_ENCODER_CABAC_ENABLE,
             static_cast<amf_int64>(AMF_VIDEO_ENCODER_CALV)) ||
        !Set(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(0))) {
      return Status::kUnsupported;
    }
    const AMF_RESULT initialized = session_.encoder()->Init(
        amf::AMF_SURFACE_NV12, config_.width, config_.height);
    if (initialized != AMF_OK) {
      return Fail(Status::kUnsupported, AmfFailure("Init AVC", initialized));
    }
    AMFSize size{};
    AMFRate rate{};
    if (session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, &size) !=
            AMF_OK ||
        size.width != config_.width || size.height != config_.height ||
        session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_FRAMERATE, &rate) !=
            AMF_OK ||
        rate.num != config_.framerate_fps || rate.den != 1) {
      return Fail(Status::kUnsupported,
                  "AMF changed requested frame dimensions/framerate");
    }
    if (!Check(AMF_VIDEO_ENCODER_USAGE,
               static_cast<amf_int64>(
                   AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY)) ||
        !Check(
            AMF_VIDEO_ENCODER_QUALITY_PRESET,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED)) ||
        !Check(AMF_VIDEO_ENCODER_PROFILE,
               static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_BASELINE)) ||
        !Check(AMF_VIDEO_ENCODER_PROFILE_LEVEL,
               static_cast<amf_int64>(config_.h264_level)) ||
        !Check(
            AMF_VIDEO_ENCODER_SCANTYPE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_SCANTYPE_PROGRESSIVE)) ||
        !Check(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true) ||
        !Check(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, false) ||
        !Check(AMF_VIDEO_ENCODER_PREENCODE_ENABLE,
               static_cast<amf_int64>(AMF_VIDEO_ENCODER_PREENCODE_DISABLED)) ||
        !Check(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
               static_cast<amf_int64>(
                   AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)) ||
        !Check(AMF_VIDEO_ENCODER_TARGET_BITRATE,
               static_cast<amf_int64>(config_.bitrate_bps)) ||
        !Check(AMF_VIDEO_ENCODER_PEAK_BITRATE,
               static_cast<amf_int64>(config_.bitrate_bps)) ||
        !ReadIntegerExactly(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
                            static_cast<amf_int64>(config_.bitrate_bps),
                            &initial_vbv_bits_) ||
        !Check(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, false) ||
        !Check(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0)) ||
        !Check(AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES,
               static_cast<amf_int64>(0)) ||
        !Check(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, static_cast<amf_int64>(1)) ||
        !Check(AMF_VIDEO_ENCODER_MAX_NUM_TEMPORAL_LAYERS,
               static_cast<amf_int64>(1)) ||
        !Check(AMF_VIDEO_ENCODER_SLICES_PER_FRAME, static_cast<amf_int64>(1)) ||
        !Check(AMF_VIDEO_ENCODER_OUTPUT_MODE,
               static_cast<amf_int64>(AMF_VIDEO_ENCODER_OUTPUT_MODE_FRAME)) ||
        !Check(AMF_VIDEO_ENCODER_CABAC_ENABLE,
               static_cast<amf_int64>(AMF_VIDEO_ENCODER_CALV)) ||
        !Check(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(0))) {
      return Status::kUnsupported;
    }
    // Read and copy while local SDK refs are alive; Open's caller closes the
    // session only after this scope has returned.
    amf::AMFVariant extra;
    if (session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &extra) !=
            AMF_OK ||
        extra.type != amf::AMF_VARIANT_INTERFACE || !extra.pInterface) {
      return Fail(Status::kUnsupported,
                  "AMF AVC Annex B extradata unavailable");
    }
    amf::AMFBufferPtr headers(extra.pInterface);
    if (!headers || headers->GetMemoryType() != amf::AMF_MEMORY_HOST ||
        !headers->GetNative() || headers->GetSize() == 0 ||
        headers->GetSize() > kMaximumPacketBytes) {
      return Fail(Status::kUnsupported, "AMF AVC extradata is not a buffer");
    }
    const ArrayView<const uint8_t> bytes(
        static_cast<const uint8_t*>(headers->GetNative()), headers->GetSize());
    if (!AnnexB(bytes) || !ValidateHeaders(bytes, config_, &pps_id_)) {
      return Fail(Status::kUnsupported,
                  "AMF AVC SPS/PPS violates Baseline/level/display contract");
    }
    headers_.assign(bytes.begin(), bytes.end());
    AllocateInputAndLog();
    return Status::kOk;
  }

#if defined(RTC_ENABLE_H265)
  Status InitializeH265Encoder() {
    if (config_.width < capability_.min_width ||
        config_.width > capability_.max_width ||
        config_.height < capability_.min_height ||
        config_.height > capability_.max_height ||
        config_.h265_level > capability_.max_h265_level ||
        config_.bitrate_bps > capability_.max_bitrate_bps) {
      return Fail(Status::kUnsupported,
                  "AMF HEVC request exceeds reported hardware capabilities");
    }
    struct IntegerSetting {
      const wchar_t* name;
      amf_int64 value;
    };
    // Usage is deliberately first: AMF applies usage defaults to subsequent
    // configuration. These settings are also verified after the driver Init.
    const IntegerSetting integers[] = {
        {AMF_VIDEO_ENCODER_HEVC_USAGE,
         AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY},
        {AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
         AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED},
        {AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN},
        {AMF_VIDEO_ENCODER_HEVC_TIER, AMF_VIDEO_ENCODER_HEVC_TIER_MAIN},
        {AMF_VIDEO_ENCODER_HEVC_PROFILE_LEVEL, config_.h265_level},
        {AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_8},
        {AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
         AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR},
        {AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, config_.bitrate_bps},
        {AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, config_.bitrate_bps},
        {AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, config_.bitrate_bps},
        {AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES, 1},
        {AMF_VIDEO_ENCODER_HEVC_MAX_LTR_FRAMES, 0},
        {AMF_VIDEO_ENCODER_HEVC_MAX_NUM_TEMPORAL_LAYERS, 1},
        {AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME, 1},
        {AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE,
         AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE_FRAME},
        // AMF documents 0 as disabling automatic IDR, not disabling GOPs.
        // Real hardware then emits CRA at a GOP boundary despite forced P.
        // Make every periodic GOP start a real IDR; the output NAL gate still
        // requires type 19/20 and rejects CRA, including requested keyframes.
        {AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, 1},
        {AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, 240},
        {AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
         AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED}};
    struct BooleanSetting {
      const wchar_t* name;
      bool value;
    };
    const BooleanSetting booleans[] = {
        {AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true},
        {AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, false},
        {AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE, false},
        {AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_SKIP_FRAME_ENABLE, false}};
    for (const auto& setting : integers) {
      if (!Set(setting.name, setting.value)) {
        return Status::kUnsupported;
      }
    }
    for (const auto& setting : booleans) {
      if (!Set(setting.name, setting.value)) {
        return Status::kUnsupported;
      }
    }
    if (!Set(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
             AMFConstructSize(config_.width, config_.height)) ||
        !Set(AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
             AMFConstructRate(config_.framerate_fps, 1))) {
      return Status::kUnsupported;
    }
    const AMF_RESULT initialized = session_.encoder()->Init(
        amf::AMF_SURFACE_NV12, config_.width, config_.height);
    if (initialized != AMF_OK) {
      return Fail(Status::kUnsupported, AmfFailure("Init HEVC", initialized));
    }
    for (const auto& setting : integers) {
      if (!Check(setting.name, setting.value)) {
        return Status::kUnsupported;
      }
    }
    for (const auto& setting : booleans) {
      if (!Check(setting.name, setting.value)) {
        return Status::kUnsupported;
      }
    }
    if (!ReadIntegerExactly(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE,
                            static_cast<amf_int64>(config_.bitrate_bps),
                            &initial_vbv_bits_)) {
      return Status::kUnsupported;
    }
    AMFSize size{};
    AMFRate rate{};
    if (session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                                        &size) != AMF_OK ||
        size.width != config_.width || size.height != config_.height ||
        session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                                        &rate) != AMF_OK ||
        rate.num != config_.framerate_fps || rate.den != 1) {
      return Fail(Status::kUnsupported,
                  "AMF HEVC changed requested frame dimensions/framerate");
    }
    amf::AMFVariant extra;
    if (session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_HEVC_EXTRADATA,
                                        &extra) != AMF_OK ||
        extra.type != amf::AMF_VARIANT_INTERFACE || !extra.pInterface) {
      return Fail(Status::kUnsupported,
                  "AMF HEVC Annex B extradata unavailable");
    }
    amf::AMFBufferPtr headers(extra.pInterface);
    if (!headers || headers->GetMemoryType() != amf::AMF_MEMORY_HOST ||
        !headers->GetNative() || headers->GetSize() == 0 ||
        headers->GetSize() > kMaximumPacketBytes) {
      return Fail(Status::kUnsupported,
                  "AMF HEVC extradata is not a host buffer");
    }
    const ArrayView<const uint8_t> bytes(
        static_cast<const uint8_t*>(headers->GetNative()), headers->GetSize());
    std::string header_error;
    if (!H265AnnexB(bytes, &header_error) ||
        !ValidateH265Headers(bytes, config_, &h265_pps_, &header_error)) {
      return Fail(Status::kUnsupported,
                  "AMF HEVC VPS/SPS/PPS violates "
                  "Main/tier/level/no-reorder/display contract: " +
                      header_error);
    }
    headers_.assign(bytes.begin(), bytes.end());
    AllocateInputAndLog();
    return Status::kOk;
  }
#endif

  void AllocateInputAndLog() {
    stride_ = (config_.width + 15) & ~15;
    const size_t luma_bytes = static_cast<size_t>(stride_) * config_.height;
    for (auto& slot : *slots_) {
      slot.pixels.assign(luma_bytes * 3 / 2, 128);
      std::memset(slot.pixels.data(), 16, luma_bytes);
      slot.available.store(true, std::memory_order_relaxed);
    }
    RTC_LOG(LS_INFO) << "AMF_CONFIG adapter=" << capability_.adapter_index
                     << " name=" << capability_.adapter_name
                     << " acceleration=" << capability_.acceleration_type
                     << " runtime=" << capability_.runtime_version
                     << " display=" << config_.width << "x" << config_.height
                     << " codec=" << (codec_ == Codec::kH264 ? "H264" : "H265")
                     << " fps=" << config_.framerate_fps << " level="
                     << static_cast<int>(codec_ == Codec::kH264
                                             ? config_.h264_level
                                             : config_.h265_level)
                     << " cbr=" << config_.bitrate_bps
                     << " noB=1 preanalysis=0 preencode=0 inputSlots="
                     << kInputSlots;
  }

  Status Fail(Status status, std::string reason) {
    error_ = std::move(reason);
    return status;
  }
  Status Abort(Status status, std::string reason) {
    Close();
    if (last_close_status_ != Status::kOk && !close_error_.empty()) {
      reason += "; cleanup: " + close_error_;
    }
    return Fail(status, std::move(reason));
  }

  const Codec codec_;
  Config config_;
  AmfH264Capability capability_;
  std::unique_ptr<std::array<InputSlot, kInputSlots>> slots_ =
      std::make_unique<std::array<InputSlot, kInputSlots>>();
  AmfSession session_;
  amf::AMFSurfacePtr input_;
  amf::AMFDataPtr output_;
  amf::AMFBufferPtr buffer_;
  std::vector<uint8_t> headers_;
  std::string error_;
  std::string close_error_;
  Status last_close_status_ = Status::kOk;
  uint32_t pps_id_ = 0;
#if defined(RTC_ENABLE_H265)
  H265PpsParser::PpsState h265_pps_;
#endif
  amf_pts next_pts_ = 0;
  amf_int64 initial_vbv_bits_ = 0;
  int stride_ = 0;
  bool initialized_ = false;
  bool need_idr_ = true;
  bool retirement_failed_ = false;
};

}  // namespace

namespace {
Status QueryAmfCapabilities(Codec codec,
                            std::vector<AmfCapability>* capabilities,
                            std::string* reason) {
  if (!capabilities) {
    if (reason) {
      *reason = "AMF capability output is null";
    }
    return Status::kInvalidArgument;
  }
  capabilities->clear();
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    if (reason) {
      *reason = "DXGI factory unavailable";
    }
    return Status::kUnavailable;
  }
  bool supported = false;
  Status status = Status::kUnavailable;
  std::string failure = "no AMD DXGI graphics adapter";
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
    if (enumerated == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(enumerated)) {
      if (reason) {
        *reason = "DXGI adapter enumeration failed";
      }
      return Status::kError;
    }
    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(adapter->GetDesc(&desc)) || desc.VendorId != kAmdVendor) {
      continue;
    }
    auto capability = DescribeAdapter(index, desc, codec);
    AmfSession session;
    Status queried = session.Open(adapter.Get(), &capability);
    std::string close_reason;
    if (session.Close(&close_reason) != Status::kOk) {
      HardwareEncoderProbes().RecordRetirementFailure(
          HardwareEncoderVendor::kAmf, close_reason);
      if (!capability.reason.empty()) {
        capability.reason += "; ";
      }
      capability.reason += close_reason;
      capability.status = Status::kRetirementFailed;
      capabilities->push_back(std::move(capability));
      if (reason) {
        *reason = capabilities->back().reason;
      }
      // Any failed retirement aborts enumeration; another adapter's success
      // cannot erase unresolved SDK ownership or permit another session.
      return Status::kRetirementFailed;
    }
    supported |= queried == Status::kOk;
    if (queried != Status::kOk) {
      status = queried;
      failure = capability.reason;
    }
    capabilities->push_back(std::move(capability));
  }
  if (reason) {
    *reason = supported ? std::string() : failure;
  }
  return supported ? Status::kOk : status;
}
}  // namespace

Status QueryAmfH264Capabilities(std::vector<AmfH264Capability>* capabilities,
                                std::string* reason) {
  if (capabilities) {
    capabilities->clear();
  }
  return HardwareEncoderProbes().Run(
      HardwareEncoderVendor::kAmf, reason,
      [capabilities](std::string* diagnostic) {
        return QueryAmfCapabilities(Codec::kH264, capabilities, diagnostic);
      });
}

std::unique_ptr<VideoEncoderBackend> CreateAmfH264Backend() {
  return std::make_unique<AmfBackend>(Codec::kH264);
}

namespace {
Status ProbeAmfH264BackendImpl(std::string* reason, uint8_t h264_level) {
  auto backend = CreateAmfH264Backend();
  Config config;
  config.codec = Codec::kH264;
  config.width = 176;
  config.height = 144;
  config.bitrate_bps = 64000;
  config.framerate_fps = 15;
  config.h264_level = h264_level;
  Status status = backend->Open(config);
  const std::string operation_error(backend->LastError());
  backend->Close();
  if (backend->LastCloseStatus() != Status::kOk) {
    if (reason) {
      *reason = operation_error;
      if (!reason->empty()) {
        *reason += "; ";
      }
      *reason += backend->LastError();
    }
    return Status::kRetirementFailed;
  }
  if (reason) {
    *reason = status == Status::kOk ? std::string() : operation_error;
  }
  return status;
}
}  // namespace

Status ProbeAmfH264Backend(std::string* reason, uint8_t h264_level) {
  return HardwareEncoderProbes().Run(HardwareEncoderVendor::kAmf, reason,
                                     [h264_level](std::string* diagnostic) {
                                       return ProbeAmfH264BackendImpl(
                                           diagnostic, h264_level);
                                     });
}

#if defined(RTC_ENABLE_H265)
Status QueryAmfH265Capabilities(std::vector<AmfH265Capability>* capabilities,
                                std::string* reason) {
  if (capabilities) {
    capabilities->clear();
  }
  return HardwareEncoderProbes().Run(
      HardwareEncoderVendor::kAmf, reason,
      [capabilities](std::string* diagnostic) {
        return QueryAmfCapabilities(Codec::kH265, capabilities, diagnostic);
      });
}

std::unique_ptr<VideoEncoderBackend> CreateAmfH265Backend() {
  return std::make_unique<AmfBackend>(Codec::kH265);
}

namespace {
Status ProbeAmfH265BackendImpl(std::string* reason, uint8_t h265_level) {
  auto backend = CreateAmfH265Backend();
  Config config;
  config.codec = Codec::kH265;
  config.width = 176;
  config.height = 144;
  config.bitrate_bps = 64000;
  config.framerate_fps = 15;
  config.h265_level = h265_level;
  Status status = backend->Open(config);
  const std::string operation_error(backend->LastError());
  backend->Close();
  if (backend->LastCloseStatus() != Status::kOk) {
    if (reason) {
      *reason = operation_error;
      if (!reason->empty()) {
        *reason += "; ";
      }
      *reason += backend->LastError();
    }
    return Status::kRetirementFailed;
  }
  if (reason) {
    *reason = status == Status::kOk ? std::string() : operation_error;
  }
  return status;
}
}  // namespace

Status ProbeAmfH265Backend(std::string* reason, uint8_t h265_level) {
  return HardwareEncoderProbes().Run(HardwareEncoderVendor::kAmf, reason,
                                     [h265_level](std::string* diagnostic) {
                                       return ProbeAmfH265BackendImpl(
                                           diagnostic, h265_level);
                                     });
}
#endif

}  // namespace webrtc::external_encoder
