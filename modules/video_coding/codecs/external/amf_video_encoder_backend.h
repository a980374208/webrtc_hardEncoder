/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_AMF_VIDEO_ENCODER_BACKEND_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_AMF_VIDEO_ENCODER_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

// Driver-reported component capabilities, not an encode/decode acceptance.
// SDK types stay private to the implementation so callers can serialize these
// fields without including AMF or linking an AMF import library.
struct AmfCapability {
  Codec codec = Codec::kH264;
  Status status = Status::kUnavailable;
  std::string reason;
  uint64_t sdk_version = 0;
  uint64_t runtime_version = 0;
  std::string runtime_path;
  uint32_t adapter_index = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint64_t dedicated_video_memory_bytes = 0;
  std::string adapter_name;
  int acceleration_type = -1;
  bool hardware_accelerated = false;
  bool nv12_input = false;
  bool host_input = false;
  bool dx11_input = false;
  int min_width = 0;
  int max_width = 0;
  int min_height = 0;
  int max_height = 0;
  int vertical_alignment = 0;
  int64_t max_bitrate_bps = 0;
  int64_t max_h264_level = 0;
  int64_t max_h265_level = 0;
  int max_bitrate_query_result = -1;
  int max_level_query_result = -1;
  // Optional caps may be absent on a usable HEVC component. -1 is unknown,
  // never a guessed hardware maximum. AMF result -1 means not queried.
  int64_t max_profile = -1;
  int64_t max_tier = -1;
  int max_profile_query_result = -1;
  int max_tier_query_result = -1;
  std::string max_profile_source = "NOT_QUERIED";
  std::string max_tier_source = "NOT_QUERIED";
  // ValidateProperty confirms only the requested Main/Main-tier values can
  // be configured without clamping. Actual Init/readback and encode remain
  // independent gates; these fields are not hardware encoding acceptance.
  int main_profile_validation_result = -1;
  int main_tier_validation_result = -1;
  int64_t main_profile_validated_value = -1;
  int64_t main_tier_validated_value = -1;
  bool main_profile_configurable = false;
  bool main_tier_configurable = false;
  std::string main_profile_configuration_source = "NOT_QUERIED";
  std::string main_tier_configuration_source = "NOT_QUERIED";
};

// Keep existing AVC callers source compatible. Capabilities are queried for
// each codec independently; support for one component says nothing about the
// other component or an actual encoding session.
using AmfH264Capability = AmfCapability;
using AmfH265Capability = AmfCapability;

// Enumerates AMD adapters and queries the real hardware AVC component. Each
// entry retains its own failure reason. No software or non-AMD fallback.
Status QueryAmfH264Capabilities(std::vector<AmfH264Capability>* capabilities,
                                std::string* reason = nullptr);

// AMD D3D11 hardware AVC only, Baseline, progressive NV12, no B frames or
// preanalysis. The runtime DLL is loaded exclusively from Windows System32.
std::unique_ptr<VideoEncoderBackend> CreateAmfH264Backend();

// Initializes a real 176x144@15 FPS / 64 kbps Baseline encoder. Does not encode
// a frame or prove synchronous completion on a particular driver.
Status ProbeAmfH264Backend(std::string* reason = nullptr,
                           uint8_t h264_level = 31);

#if defined(RTC_ENABLE_H265)
Status QueryAmfH265Capabilities(std::vector<AmfH265Capability>* capabilities,
                                std::string* reason = nullptr);

// AMD D3D11 hardware HEVC, 8-bit Main/Main tier, single temporal layer and
// reference picture, no frame reordering, progressive NV12 input.
std::unique_ptr<VideoEncoderBackend> CreateAmfH265Backend();

// Initializes a real HEVC component but does not prove encoding or decoding.
Status ProbeAmfH265Backend(std::string* reason = nullptr,
                           uint8_t h265_level = 93);
#endif

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_AMF_VIDEO_ENCODER_BACKEND_H_
