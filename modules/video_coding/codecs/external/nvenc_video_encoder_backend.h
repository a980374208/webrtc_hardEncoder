/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 * Use of this source code is governed by a BSD-style license
 * that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_BACKEND_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>

#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

// Windows D3D11 backends for H.264 Baseline and 8-bit HEVC Main/Main tier.
// No CUDA, OBS, or NvCodec C++ runtime is required.
std::unique_ptr<VideoEncoderBackend> CreateNvencH264Backend();
std::unique_ptr<VideoEncoderBackend> CreateNvencH265Backend();

// Opens a real hardware session (320x240 H.264; 160x128 HEVC); header/DLL
// presence alone is not a capability result. This does not prove successful
// encoding or decoding. The HEVC probe preserves the requested SDP level.
Status ProbeNvencH264Backend(std::string* reason);
Status ProbeNvencH265Backend(std::string* reason, uint8_t level = 93);

}  // namespace webrtc::external_encoder

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_BACKEND_H_
