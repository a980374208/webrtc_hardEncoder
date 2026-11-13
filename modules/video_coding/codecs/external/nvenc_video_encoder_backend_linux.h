/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_BACKEND_LINUX_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_NVENC_VIDEO_ENCODER_BACKEND_LINUX_H_

#include <memory>
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device_internal.h"

#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder {

// Linux-only H.264 Baseline/CAVLC backend, borrowing CPU NV12 input until the
// synchronous call returns. CUDA and NVENC are loaded only by Open(). An ordinal
// of -1 enumerates devices in stable PCI order; an explicit ordinal never falls
// back to a different device. The driver calls have no hard cancellation or
// deadline contract: a caller requiring a bounded wait must own frames and the
// backend on a separate sequence, retaining that owner after a timeout.
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackend(
    int device_ordinal = -1);

std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackend(
    const LinuxNvencDeviceSelection& selection);
namespace internal {
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForProbe(
    int ordinal, std::shared_ptr<LinuxNvencDeviceCollector> collector);
}
}  // namespace webrtc::external_encoder
#endif
