/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_FFMPEG_H265_TEST_DECODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_FFMPEG_H265_TEST_DECODER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "api/video_codecs/video_decoder.h"

namespace webrtc {

// Test-only reference receiver for HEVC Main / 8-bit / 4:2:0 access units.
// The absolute directory must contain the OBS FFmpeg 61/59 DLLs and their
// dependencies. Dynamic binding isolates them from WebRTC's static FFmpeg.
// Configure, Decode, callback registration, and Release use one decoder
// sequence. Callbacks run synchronously on that sequence; there are no tasks.
class FfmpegH265TestDecoder final : public VideoDecoder {
 public:
  explicit FfmpegH265TestDecoder(std::string absolute_dll_directory);
  ~FfmpegH265TestDecoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const EncodedImage& image, int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

  // Diagnostic text, read on the decoder sequence after a failed operation.
  const std::string& last_error() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_FFMPEG_H265_TEST_DECODER_H_
