/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_HARDWARE_ENCODER_DEVICE_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_HARDWARE_ENCODER_DEVICE_H_
#define WEBRTC_LINUX_HARDWARE_DEVICE_SELECTION 1
#include <cstdint>
#include <string>
namespace webrtc::external_encoder {
// Owned probe results, valid only for the probed machine/driver generation.
// Typed Create revalidates the actual device; disappearance or identity drift
// fails this selection instead of silently choosing a replacement device.
struct LinuxNvencDeviceSelection {
  int cuda_ordinal = -1;
  std::string pci_bus_id;
  uint32_t cuda_driver_version = 0;  // Unknown until queried, not synthesized.
  uint32_t nvenc_api_version = 0;  // Driver max API: (major << 4) | minor.
};
struct LinuxQsvDeviceSelection {
  std::string render_node;
  uint16_t vendor_id = 0;
  uint16_t device_id = 0;
  uint32_t pci_domain = 0;
  uint8_t pci_bus = 0;
  uint8_t pci_device = 0;
  uint8_t pci_function = 0;
  uint16_t vpl_api_major = 0;
  uint16_t vpl_api_minor = 0;
  uint16_t va_api_major = 0;
  uint16_t va_api_minor = 0;
};
struct LinuxVaapiDeviceSelection {
  std::string render_node;
  uint16_t vendor_id = 0;
  uint16_t device_id = 0;
  uint32_t pci_domain = 0;
  uint8_t pci_bus = 0;
  uint8_t pci_device = 0;
  uint8_t pci_function = 0;
};
}  // namespace webrtc::external_encoder
#endif
