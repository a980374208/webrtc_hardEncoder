/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_BACKEND_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>

#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

// Intel D3D11 hardware only. The oneVPL dispatcher may bridge to the driver's
// legacy Media SDK runtime. Link a dispatcher built with the application's CRT.
std::unique_ptr<VideoEncoderBackend> CreateQsvH264Backend();
// HEVC Main, Main tier, 8-bit NV12, one layer and no B-frame reordering.
std::unique_ptr<VideoEncoderBackend> CreateQsvH265Backend();

// Queries and initializes a real 176x144@15 FPS, 64 kbps hardware encoder at
// the requested Baseline level. This does not prove encoding or decoding.
Status ProbeQsvH264Backend(std::string* reason, uint8_t h264_level = 31);

// The probe fits HEVC Main level 1 (160x128@15 FPS, 64 kbps). `h265_level`
// uses the RTP/SPS general_level_idc representation, e.g. 93 for level 3.1.
Status ProbeQsvH265Backend(std::string* reason, uint8_t h265_level = 93);

}  // namespace webrtc::external_encoder

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_BACKEND_H_
