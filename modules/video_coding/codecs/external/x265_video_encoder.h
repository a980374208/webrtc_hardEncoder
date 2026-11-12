/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_X265_VIDEO_ENCODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_X265_VIDEO_ENCODER_H_
#include <memory>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
namespace webrtc {
// HEVC Main/Main tier, software encoder using the shared WebRTC adapter.
// Defaults retain one frame thread/no WPP. Experiments opt in with the exact
// WebRTC-X265-ExperimentalTuning profile; unknown profiles fail creation.
std::unique_ptr<VideoEncoder> CreateX265VideoEncoder(
    const Environment& env,
    const SdpVideoFormat& format);
bool IsX265VideoEncoderAvailable(const SdpVideoFormat& format);
}  // namespace webrtc
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_X265_VIDEO_ENCODER_H_
