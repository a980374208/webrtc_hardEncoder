/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 * Use of this source code is governed by a BSD-style license
 * that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_VIDEO_ENCODER_BACKEND_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_VIDEO_ENCODER_BACKEND_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace webrtc::external_encoder {

enum class Codec { kH264, kH265 };
enum class Status {
  kOk,
  kUnavailable,
  kUnsupported,
  kInvalidArgument,
  kNoOutput,
  kTimeout,
  kError,
  // SDK cleanup failed: ownership remains unconfirmed. This is distinct from
  // an ordinary unsupported/unavailable/probe error and forbids fallback.
  kRetirementFailed,
};

struct Config {
  Codec codec = Codec::kH264;
  int width = 0;
  int height = 0;
  uint32_t bitrate_bps = 0;
  uint32_t framerate_fps = 0;
  // H.264 level_idc, e.g. 31 for level 3.1. The first backend uses Baseline.
  uint8_t h264_level = 31;
  // HEVC general_level_idc (30 times the level), Main profile and Main tier.
  uint8_t h265_level = 93;
};

struct Rates {
  uint32_t bitrate_bps = 0;
  uint32_t framerate_fps = 0;
};

// Borrowed CPU NV12 planes. The backend must not retain them after Encode.
struct Frame {
  const uint8_t* y = nullptr;
  const uint8_t* uv = nullptr;
  int stride_y = 0;
  int stride_uv = 0;
  int width = 0;
  int height = 0;
  uint64_t id = 0;
};

// Exactly one complete Annex B access unit; output storage belongs to caller.
struct Packet {
  std::vector<uint8_t> bytes;
  uint64_t frame_id = 0;
  bool key_frame = false;
  int qp = -1;
};

struct Info {
  std::string implementation_name;
  bool is_hardware_accelerated = false;
};

// Calls are serialized by the adapter on its encoder sequence. There are no
// backend callbacks, and successful Encode completes the corresponding frame
// before returning. Resolution/profile changes require Close then Open.
// Close is idempotent and retires SDK resources before releasing host storage.
// Synchronous completion can block inside vendor/driver calls; Encode and Close
// do not provide a hard wall-clock deadline or cancellation guarantee.
class VideoEncoderBackend {
 public:
  virtual ~VideoEncoderBackend() = default;
  virtual Info GetInfo() const = 0;
  virtual Status Open(const Config& config) = 0;
  virtual Status SetRates(const Rates& rates) = 0;
  virtual Status Encode(const Frame& frame,
                        bool force_key_frame,
                        Packet* packet) = 0;
  virtual void Close() = 0;
  // Result of the last Close, including SDK retirement failures. Backends
  // without fallible cleanup can retain the default. A failed close must be
  // reported by Release and must not be counted as successful retirement.
  virtual Status LastCloseStatus() const { return Status::kOk; }
  virtual std::string_view LastError() const = 0;
};

}  // namespace webrtc::external_encoder

#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_VIDEO_ENCODER_BACKEND_H_
