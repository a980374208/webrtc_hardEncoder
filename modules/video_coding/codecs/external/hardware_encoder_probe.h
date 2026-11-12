/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_HARDWARE_ENCODER_PROBE_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_HARDWARE_ENCODER_PROBE_H_

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

enum class HardwareEncoderVendor { kNvenc, kQsv, kAmf };

// Serialize actual capability probes, including their cleanup. A failed
// retirement poisons that vendor for this process, across codecs and new
// backend instances. Ordinary errors remain retryable. Probe callbacks may
// Check/Record state as actual Open/Close does, but must not recursively Run.
class HardwareEncoderProbeCoordinator {
 public:
  Status Run(HardwareEncoderVendor vendor,
             std::string* reason,
             const std::function<Status(std::string*)>& probe);
  Status Check(HardwareEncoderVendor vendor,
               std::string* reason = nullptr) const;
  Status CheckAny(std::string* reason = nullptr) const;
  void RecordRetirementFailure(HardwareEncoderVendor vendor,
                               std::string_view reason);

 private:
  struct RetirementFailure {
    bool failed = false;
    std::string reason;
  };
  // Never hold state_mutex_ while invoking vendor code. Open/Close publish and
  // inspect state during Run without reacquiring the serialization mutex.
  std::mutex probe_mutex_;
  mutable std::mutex state_mutex_;
  std::array<RetirementFailure, 3> failures_;
};

HardwareEncoderProbeCoordinator& HardwareEncoderProbes();

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_HARDWARE_ENCODER_PROBE_H_
