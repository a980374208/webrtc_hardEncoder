/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_nvenc_video_encoder.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "modules/video_coding/codecs/external/backend_video_encoder.h"
#include "modules/video_coding/codecs/external/hardware_encoder_probe.h"
#include "modules/video_coding/codecs/external/linux_bounded_video_encoder_backend.h"
#include "modules/video_coding/codecs/external/nvenc_video_encoder_backend_linux.h"

namespace webrtc {
std::unique_ptr<VideoEncoder> CreateLinuxNvencVideoEncoder(
    const Environment&, const SdpVideoFormat& format, int device_ordinal) {
  using namespace external_encoder;
  if (!IsBackendH264FormatSupported(format) || device_ordinal < -1 ||
      CheckLinuxBackendRetirement() != Status::kOk) return nullptr;
  return CreateBackendVideoEncoder(format, CreateLinuxBoundedBackend(
      CreateLinuxNvencBackend(device_ordinal), HardwareEncoderVendor::kNvenc,
      {"NVENC-CUDA-Linux", true}));
}
std::unique_ptr<VideoEncoder> CreateLinuxNvencVideoEncoder(
    const Environment&, const SdpVideoFormat& format,
    const external_encoder::LinuxNvencDeviceSelection& selection) {
  using namespace external_encoder;
  if (!IsBackendH264FormatSupported(format) || CheckLinuxBackendRetirement() != Status::kOk)
    return nullptr;
  return CreateBackendVideoEncoder(format, CreateLinuxBoundedBackend(
      CreateLinuxNvencBackend(selection), HardwareEncoderVendor::kNvenc,
      {"NVENC-CUDA-Linux", true}));
}
external_encoder::Status ProbeLinuxNvencVideoEncoderAvailability(
    const SdpVideoFormat& format, int device_ordinal, std::string* reason) {
  return ProbeLinuxNvencVideoEncoderAvailability(format, device_ordinal, reason, nullptr);
}
external_encoder::Status ProbeLinuxNvencVideoEncoderAvailability(
    const SdpVideoFormat& format, int device_ordinal, std::string* reason,
    external_encoder::LinuxNvencDeviceSelection* resolved) {
  using namespace external_encoder;
  if (resolved) *resolved = {};
  if (!IsBackendH264FormatSupported(format) || device_ordinal < -1) {
    if (reason) *reason = "unsupported_linux_nvenc_format_or_device";
    return Status::kUnsupported;
  }
  const auto profile = ParseSdpForH264ProfileLevelId(format.parameters);
  if (!profile || profile->level == H264Level::kLevel1_b) {
    if (reason) *reason = "unsupported_linux_nvenc_level";
    return Status::kUnsupported;
  }
  auto collector = std::make_shared<external_encoder::internal::LinuxNvencDeviceCollector>();
  auto status = HardwareEncoderProbes().Run(HardwareEncoderVendor::kNvenc, reason,
      [device_ordinal, level = static_cast<uint8_t>(profile->level), collector](std::string* diagnostic) {
        auto backend = CreateLinuxBoundedBackend(
            external_encoder::internal::CreateLinuxNvencBackendForProbe(device_ordinal, collector),
            HardwareEncoderVendor::kNvenc, {"NVENC-CUDA-Linux", true});
        Config config;
        config.width = 160; config.height = 128; config.bitrate_bps = 64000;
        config.framerate_fps = 15; config.h264_level = level;
        auto result = backend->Open(config);
        backend->Close();
        if (backend->LastCloseStatus() != Status::kOk) result = Status::kRetirementFailed;
        if (diagnostic) diagnostic->assign(backend->LastError());
        backend.reset();  // Includes bounded inner destruction before publishing.
        return CheckLinuxBackendRetirement() == Status::kOk ? result : Status::kRetirementFailed;
      });
  if (CheckLinuxBackendRetirement() != Status::kOk) {
    if (reason) *reason = "linux_backend_retirement_failed";
    return Status::kRetirementFailed;
  }
  if (status == Status::kOk) {
    auto selected = collector->RetiredSelection();
    if (!selected) {
      if (reason) *reason = "linux_nvenc_resolved_device_unconfirmed";
      return Status::kError;
    }
    if (resolved) *resolved = std::move(*selected);
  }
  return status;
}
}  // namespace webrtc
