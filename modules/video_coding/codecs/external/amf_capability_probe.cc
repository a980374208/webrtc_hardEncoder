/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "modules/video_coding/codecs/external/amf_video_encoder_backend.h"

namespace {
std::string Quote(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 32) {
      const char hex[] = "0123456789abcdef";
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += static_cast<char>(c);
    }
  }
  return result + "\"";
}
std::string Version(uint64_t value) {
  std::string result;
  for (int shift : {48, 32, 16, 0}) {
    if (!result.empty())
      result += ".";
    result += std::to_string((value >> shift) & 0xffff);
  }
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace webrtc::external_encoder;
  std::string codec = "H264";
  std::string output_path;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--output" && i + 1 < argc && output_path.empty()) {
      output_path = argv[++i];
    } else if (argument == "--codec" && i + 1 < argc) {
      codec = argv[++i];
    } else {
      std::cerr << "Usage: amf_capability_probe [--codec H264|H265] --output "
                   "NEW_JSON\n";
      return 2;
    }
  }
  if (output_path.empty() || (codec != "H264" && codec != "H265")) {
    std::cerr << "Usage: amf_capability_probe [--codec H264|H265] --output "
                 "NEW_JSON\n";
    return 2;
  }
  std::ifstream existing(output_path);
  if (existing.good())
    return 2;
  std::vector<AmfCapability> capabilities;
  std::string reason, initialization_reason;
  Status queried;
  Status initialized;
  if (codec == "H264") {
    queried = QueryAmfH264Capabilities(&capabilities, &reason);
    if (queried == Status::kRetirementFailed) {
      initialized = queried;
      initialization_reason =
          "Initialization blocked after capability cleanup failure: " + reason;
    } else {
      initialized = ProbeAmfH264Backend(&initialization_reason);
    }
  } else {
#if defined(RTC_ENABLE_H265)
    queried = QueryAmfH265Capabilities(&capabilities, &reason);
    if (queried == Status::kRetirementFailed) {
      initialized = queried;
      initialization_reason =
          "Initialization blocked after capability cleanup failure: " + reason;
    } else {
      initialized = ProbeAmfH265Backend(&initialization_reason);
    }
#else
    std::cerr << "H265 capability probe was not compiled\n";
    return 3;
#endif
  }
  std::ofstream output(output_path);
  output << "{\n\"schema_version\":3,\"codec\":" << Quote(codec)
         << ",\"query_status\":" << static_cast<int>(queried)
         << ",\"query_reason\":" << Quote(reason)
         << ",\"initialization_status\":" << static_cast<int>(initialized)
         << ",\"initialization_reason\":" << Quote(initialization_reason)
         << ",\"real_encoding\":\"NOT_RUN\",\"adapters\":[";
  bool first = true;
  for (const auto& c : capabilities) {
    if (!first)
      output << ",";
    first = false;
    output << "{\"status\":" << static_cast<int>(c.status)
           << ",\"reason\":" << Quote(c.reason)
           << ",\"adapter_index\":" << c.adapter_index
           << ",\"name\":" << Quote(c.adapter_name)
           << ",\"vendor_id\":" << c.vendor_id
           << ",\"device_id\":" << c.device_id
           << ",\"dedicated_video_memory_bytes\":"
           << c.dedicated_video_memory_bytes
           << ",\"sdk_version\":" << Quote(Version(c.sdk_version))
           << ",\"runtime_version\":" << Quote(Version(c.runtime_version))
           << ",\"runtime_path\":" << Quote(c.runtime_path)
           << ",\"acceleration_type\":" << c.acceleration_type
           << ",\"hardware_accelerated\":"
           << (c.hardware_accelerated ? "true" : "false")
           << ",\"nv12_input\":" << (c.nv12_input ? "true" : "false")
           << ",\"host_input\":" << (c.host_input ? "true" : "false")
           << ",\"dx11_input\":" << (c.dx11_input ? "true" : "false")
           << ",\"min_width\":" << c.min_width
           << ",\"max_width\":" << c.max_width
           << ",\"min_height\":" << c.min_height
           << ",\"max_height\":" << c.max_height
           << ",\"vertical_alignment\":" << c.vertical_alignment
           << ",\"max_bitrate_bps\":" << c.max_bitrate_bps
           << ",\"max_h264_level\":" << c.max_h264_level
           << ",\"max_h265_level\":" << c.max_h265_level
           << ",\"max_bitrate_query_result\":" << c.max_bitrate_query_result
           << ",\"max_level_query_result\":" << c.max_level_query_result
           << ",\"max_profile\":" << c.max_profile
           << ",\"max_tier\":" << c.max_tier
           << ",\"max_profile_query_result\":" << c.max_profile_query_result
           << ",\"max_tier_query_result\":" << c.max_tier_query_result
           << ",\"max_profile_source\":" << Quote(c.max_profile_source)
           << ",\"max_tier_source\":" << Quote(c.max_tier_source)
           << ",\"main_profile_validation_result\":"
           << c.main_profile_validation_result
           << ",\"main_tier_validation_result\":"
           << c.main_tier_validation_result
           << ",\"main_profile_validated_value\":"
           << c.main_profile_validated_value
           << ",\"main_tier_validated_value\":" << c.main_tier_validated_value
           << ",\"main_profile_configurable\":"
           << (c.main_profile_configurable ? "true" : "false")
           << ",\"main_tier_configurable\":"
           << (c.main_tier_configurable ? "true" : "false")
           << ",\"main_profile_configuration_source\":"
           << Quote(c.main_profile_configuration_source)
           << ",\"main_tier_configuration_source\":"
           << Quote(c.main_tier_configuration_source) << "}";
  }
  output << "]\n}\n";
  output.close();
  if (!output)
    return 1;
  std::cout << "CAPABILITY_QUERY codec=" << codec
            << " status=" << static_cast<int>(queried)
            << " INITIALIZATION status=" << static_cast<int>(initialized)
            << " hardware_encode=NOT_RUN\n";
  return queried == Status::kOk && initialized == Status::kOk ? 0 : 1;
}
