/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_H_

#include <memory>
#include <string>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc {

// H.264 retains the existing OpenH264 fallback. H.265 Main retains same-codec
// x265 fallback when it is built. Support requires an actual device probe for
// the chosen codec; failed SDK retirement must not be treated as unavailable.
std::unique_ptr<VideoEncoder> CreateNvencVideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format,
    external_encoder::Status* probe_status = nullptr);
external_encoder::Status ProbeNvencVideoEncoderAvailability(
    const SdpVideoFormat& format,
    std::string* reason = nullptr);
bool IsNvencVideoEncoderAvailable(const SdpVideoFormat& format);

}  // namespace webrtc
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_H_
