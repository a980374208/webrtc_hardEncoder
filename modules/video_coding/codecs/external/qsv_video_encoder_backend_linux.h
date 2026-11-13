/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_BACKEND_LINUX_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_QSV_VIDEO_ENCODER_BACKEND_LINUX_H_
#include <memory>
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device_internal.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"
namespace webrtc::external_encoder {
// Raw synchronous Linux backend; production entry points always wrap this in
// CreateLinuxBoundedBackend. Requires oneVPL extended PCI identity, VAAPI and
// CPU NV12 system-memory support. No native surface import or software MFX.
// -1 tries stable Intel PCI order; an explicit ordinal never switches device.
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackend(int device_ordinal = -1);
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackend(
    const LinuxQsvDeviceSelection& selection);
namespace internal {
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForProbe(
    int ordinal, std::shared_ptr<LinuxQsvDeviceCollector> collector);
}
}  // namespace webrtc::external_encoder
#endif
