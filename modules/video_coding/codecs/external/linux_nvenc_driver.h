/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_NVENC_DRIVER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_NVENC_DRIVER_H_

#include <memory>
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device_internal.h"
#include <string>

#include "dynlink_cuda.h"
#include "nvEncodeAPI.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder::internal {

// Linux-private, typed dispatch table. Shared ownership keeps both DSOs loaded
// until every context/session which can call them has been retired.
struct LinuxNvencDriver {
  LinuxNvencDriver() = default;
  ~LinuxNvencDriver();
  LinuxNvencDriver(const LinuxNvencDriver&) = delete;
  LinuxNvencDriver& operator=(const LinuxNvencDriver&) = delete;

  bool HasRequiredFunctions() const;

  tcuInit* init = nullptr;
  tcuDeviceGetCount* device_get_count = nullptr;
  tcuDeviceGet* device_get = nullptr;
  tcuDeviceGetPCIBusId* device_get_pci_bus_id = nullptr;
  tcuCtxCreate_v2* context_create = nullptr;
  tcuCtxGetCurrent* context_get_current = nullptr;
  tcuCtxGetDevice* context_get_device = nullptr;
  tcuCtxPushCurrent_v2* context_push = nullptr;
  tcuCtxPopCurrent_v2* context_pop = nullptr;
  tcuCtxDestroy_v2* context_destroy = nullptr;
  tcuMemAllocPitch_v2* memory_alloc_pitch = nullptr;
  tcuMemFree_v2* memory_free = nullptr;
  tcuMemcpy2D_v2* copy_2d = nullptr;
  NV_ENCODE_API_FUNCTION_LIST encode = {};
  uint32_t driver_api_version = 0;

  // Owned only by the production loader; injected test tables leave these null.
  void* cuda_library = nullptr;
  void* encode_library = nullptr;
};

std::shared_ptr<LinuxNvencDriver> LoadLinuxNvencDriver(Status* status,
                                                     std::string* reason);
// Internal loader seam, never consumed by device selection or product config.
std::shared_ptr<LinuxNvencDriver> LoadLinuxNvencDriverForTesting(
    const char* cuda_library,
    const char* encode_library,
    Status* status,
    std::string* reason);

std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForTesting(
    std::shared_ptr<LinuxNvencDriver> driver,
    int device_ordinal = -1);
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForTesting(
    std::shared_ptr<LinuxNvencDriver> driver, int device_ordinal,
    std::shared_ptr<LinuxNvencDeviceCollector> collector);
std::unique_ptr<VideoEncoderBackend> CreateLinuxNvencBackendForTesting(
    std::shared_ptr<LinuxNvencDriver> driver,
    const LinuxNvencDeviceSelection& selection);

}  // namespace webrtc::external_encoder::internal
#endif
