/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_NVENC_VIDEO_ENCODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_NVENC_VIDEO_ENCODER_H_

#include <memory>
#include <string>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device.h"

namespace webrtc {

// Linux-only, hardware-only H264 Baseline/L1T1. Creation does not probe a driver;
// InitEncode and actual encoded output are separate availability witnesses.
// -1 enumerates stable device candidates; an explicit ordinal never changes GPU.
// Linux-only backend worker bounds caller waits (candidate defaults in
// linux_bounded_video_encoder_backend.h). No driver calls run on the caller.
// Timeout cannot cancel a driver: it seals/poisons NVENC with retirement failure
// and retains worker resources until owner cleanup; late output is discarded.
std::unique_ptr<VideoEncoder> CreateLinuxNvencVideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format,
    int device_ordinal = -1);

// Open/Close discovery only: neither encoding, RTP nor successful device output.
// Run on a background owner. Open/Close caller waits are bounded separately;
// underlying driver calls cannot be canceled. Timeout or failed retirement
// poisons NVENC for this process and forbids ordinary software fallback.
external_encoder::Status ProbeLinuxNvencVideoEncoderAvailability(
    const SdpVideoFormat& format,
    int device_ordinal = -1,
    std::string* reason = nullptr);

// Frozen device choice from a successful, confirmed-retired availability probe.
// InitEncode revalidates the actual ordinal/path-to-PCI identity; no Auto retry.
std::unique_ptr<VideoEncoder> CreateLinuxNvencVideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format,
    const external_encoder::LinuxNvencDeviceSelection& selection);
// The caller output is written only after Open, Close and owner destruction
// complete successfully. No caller pointer is passed to a late worker.
external_encoder::Status ProbeLinuxNvencVideoEncoderAvailability(
    const SdpVideoFormat& format,
    int device_ordinal,
    std::string* reason,
    external_encoder::LinuxNvencDeviceSelection* resolved);

}  // namespace webrtc
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_NVENC_VIDEO_ENCODER_H_
