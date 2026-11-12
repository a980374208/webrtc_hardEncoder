/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_H_

#include <memory>
#include <string>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc {

// H.264 Baseline/mode 1 retains the existing OpenH264 fallback. HEVC Main/Main
// tier retains same-codec x265 fallback when it is built. Failed SDK retirement
// must not be treated as ordinary hardware unavailability.
std::unique_ptr<VideoEncoder> CreateQsvVideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format,
    external_encoder::Status* probe_status = nullptr);
external_encoder::Status ProbeQsvVideoEncoderAvailability(
    const SdpVideoFormat& format,
    std::string* reason = nullptr);
bool IsQsvVideoEncoderAvailable(const SdpVideoFormat& format);

}  // namespace webrtc
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_H_
