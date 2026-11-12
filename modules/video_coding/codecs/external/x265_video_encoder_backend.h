/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_X265_VIDEO_ENCODER_BACKEND_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_X265_VIDEO_ENCODER_BACKEND_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

// Experimental opt-in only. Frame threading remains one, so each Encode
// synchronously retires its input; WPP adds workers inside that one picture.
// Keep the default unchanged until performance, quality and rate control are
// measured. Separate processes isolate different candidates during measurement.
struct X265Tuning {
  int wpp_pool_threads = 0;  // Supported worker requests: 0, 4, 8, 16.
  bool strict_cbr = true;
  int min_cu_size = 8;  // Supported values: 8, 16.
  bool split_rd_skip = false;
};

// Empty or "baseline" retains the product default. Unknown names fail closed.
std::optional<X265Tuning> ParseX265ExperimentalTuning(std::string_view name);

// Real CPU x265 encoding: Main, Main tier, 8-bit 4:2:0, one layer, no B frames
// or lookahead. Link a matching 8-bit x265 build and application CRT. The x265
// dependency has its own GPL/commercial licensing; it is not an FFmpeg fixture.
std::unique_ptr<VideoEncoderBackend> CreateX265H265Backend(
    X265Tuning tuning = {});

// Opens a real 160x128@15 FPS, 64 kbps software encoder at the requested level.
// This checks the active SPS and configuration, but does not encode a frame.
Status ProbeX265H265Backend(std::string* reason = nullptr,
                            uint8_t h265_level = 93);

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_X265_VIDEO_ENCODER_BACKEND_H_
