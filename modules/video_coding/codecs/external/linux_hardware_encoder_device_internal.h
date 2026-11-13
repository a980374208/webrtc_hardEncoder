/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_HARDWARE_ENCODER_DEVICE_INTERNAL_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_HARDWARE_ENCODER_DEVICE_INTERNAL_H_
#include <mutex>
#include <optional>
#include <utility>
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device.h"
namespace webrtc::external_encoder::internal {
// Shared by probe caller and backend owner. A timed-out worker keeps its own
// strong reference; it never writes into the probe caller's stack/output.
// No result becomes readable until that exact Open has confirmed Close.
template <typename Selection>
class LinuxDeviceSelectionCollector {
 public:
  void BeginOpen() {
    std::lock_guard<std::mutex> lock(mutex_); pending_.reset(); retired_.reset();
  }
  void Opened(Selection value) {
    std::lock_guard<std::mutex> lock(mutex_); pending_ = std::move(value);
  }
  void Closed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) { retired_ = std::move(pending_); pending_.reset(); }
  }
  void FailedRetirement() {
    std::lock_guard<std::mutex> lock(mutex_); retired_.reset();
  }
  std::optional<Selection> RetiredSelection() const {
    std::lock_guard<std::mutex> lock(mutex_); return retired_;
  }
 private:
  mutable std::mutex mutex_;
  std::optional<Selection> pending_, retired_;
};
using LinuxNvencDeviceCollector = LinuxDeviceSelectionCollector<LinuxNvencDeviceSelection>;
using LinuxQsvDeviceCollector = LinuxDeviceSelectionCollector<LinuxQsvDeviceSelection>;
using LinuxVaapiDeviceCollector = LinuxDeviceSelectionCollector<LinuxVaapiDeviceSelection>;
}  // namespace webrtc::external_encoder::internal
#endif
