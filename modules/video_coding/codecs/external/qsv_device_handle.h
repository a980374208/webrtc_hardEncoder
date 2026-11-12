/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_DEVICE_HANDLE_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_DEVICE_HANDLE_H_

#include <wrl/client.h>

#include <cstdint>

namespace webrtc::external_encoder {

// The Media SDK compatibility path treats GetHandle as borrowed, as legacy
// FFmpeg callers do. Keep our validation reference independent on API 1.x;
// API 2 follows the documented added COM reference. The exact legacy driver's
// ownership behavior still needs hardware confirmation.
template <typename Device>
Microsoft::WRL::ComPtr<Device> RetainQsvDeviceHandle(
    Device* device,
    uint16_t runtime_api_major) {
  if (runtime_api_major == 1) {
    return Microsoft::WRL::ComPtr<Device>(device);
  }
  Microsoft::WRL::ComPtr<Device> owned;
  owned.Attach(device);
  return owned;
}

}  // namespace webrtc::external_encoder
#endif  // MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_DEVICE_HANDLE_H_
