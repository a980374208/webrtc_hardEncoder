/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_BACKEND_VIDEO_ENCODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_BACKEND_VIDEO_ENCODER_H_

#include <memory>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc {

// H.264 Baseline, packetization-mode=1, and a single temporal/spatial layer.
bool IsBackendH264FormatSupported(const SdpVideoFormat& format);
// Compatibility entry point for existing NVENC callers.
bool IsNvencH264FormatSupported(const SdpVideoFormat& format);
// HEVC Main, Main tier, 8-bit NV12, SRST with no DON interleaving; one layer.
bool IsBackendH265FormatSupported(const SdpVideoFormat& format);
// Compatibility entry point for existing NVENC callers.
bool IsNvencH265FormatSupported(const SdpVideoFormat& format);
// Opt in only when the caller provides a software encoder for the same codec.
// Existing HEVC callers retain their codec-switch failure semantics.
enum class BackendFailurePolicy { kDefault, kSameCodecSoftwareFallback };
std::unique_ptr<VideoEncoder> CreateBackendVideoEncoder(
    const SdpVideoFormat& format,
    std::unique_ptr<external_encoder::VideoEncoderBackend> backend,
    BackendFailurePolicy failure_policy = BackendFailurePolicy::kDefault);

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_BACKEND_VIDEO_ENCODER_H_
