/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_QSV_DRIVER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_QSV_DRIVER_H_
#include <memory>
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device_internal.h"
#include <string>
#include <va/va.h>
#include <va/va_drm.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <xf86drm.h>
#include "modules/video_coding/codecs/external/video_encoder_backend.h"

namespace webrtc::external_encoder::internal {
// Linux-only typed dispatch. DSOs remain loaded through session/display/FD
// retirement; fake tests inject exactly this ABI, without a parallel backend.
struct LinuxQsvDriver {
  ~LinuxQsvDriver();
  LinuxQsvDriver() = default;
  LinuxQsvDriver(const LinuxQsvDriver&) = delete;
  LinuxQsvDriver& operator=(const LinuxQsvDriver&) = delete;
  bool HasRequiredFunctions() const;
  decltype(&MFXLoad) load = nullptr;
  decltype(&MFXUnload) unload = nullptr;
  decltype(&MFXCreateConfig) create_config = nullptr;
  decltype(&MFXSetConfigFilterProperty) filter = nullptr;
  decltype(&MFXEnumImplementations) enumerate = nullptr;
  decltype(&MFXDispReleaseImplDescription) release_description = nullptr;
  decltype(&MFXCreateSession) create_session = nullptr;
  decltype(&MFXClose) close_session = nullptr;
  decltype(&MFXQueryIMPL) query_impl = nullptr;
  decltype(&MFXQueryVersion) query_version = nullptr;
  decltype(&MFXVideoCORE_SetHandle) set_handle = nullptr;
  decltype(&MFXVideoCORE_GetHandle) get_handle = nullptr;
  decltype(&MFXVideoENCODE_Query) query = nullptr;
  decltype(&MFXVideoENCODE_QueryIOSurf) query_surfaces = nullptr;
  decltype(&MFXVideoENCODE_Init) initialize = nullptr;
  decltype(&MFXVideoENCODE_Reset) reset = nullptr;
  decltype(&MFXVideoENCODE_GetVideoParam) get_parameters = nullptr;
  decltype(&MFXVideoENCODE_EncodeFrameAsync) encode = nullptr;
  decltype(&MFXVideoCORE_SyncOperation) sync = nullptr;
  decltype(&MFXVideoENCODE_Close) close_encoder = nullptr;
  decltype(&vaGetDisplayDRM) display = nullptr;
  decltype(&vaInitialize) initialize_va = nullptr;
  decltype(&vaTerminate) terminate_va = nullptr;
  decltype(&drmGetDevices2) get_devices = nullptr;
  decltype(&drmFreeDevices) free_devices = nullptr;
  decltype(&drmGetDevice2) get_device = nullptr;
  decltype(&drmFreeDevice) free_device = nullptr;
  int (*open_node)(const char*) = nullptr;
  int (*close_node)(int) = nullptr;
  void* vpl_library = nullptr;
  void* va_library = nullptr;
  void* va_drm_library = nullptr;
  void* drm_library = nullptr;
};
std::shared_ptr<LinuxQsvDriver> LoadLinuxQsvDriver(Status*, std::string*);
std::shared_ptr<LinuxQsvDriver> LoadLinuxQsvDriverForTesting(
    const char* vpl, const char* va, const char* va_drm, const char* drm,
    Status*, std::string*);
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForTesting(
    std::shared_ptr<LinuxQsvDriver>, int device_ordinal = -1);
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForTesting(
    std::shared_ptr<LinuxQsvDriver>, int device_ordinal,
    std::shared_ptr<LinuxQsvDeviceCollector> collector);
std::unique_ptr<VideoEncoderBackend> CreateLinuxQsvBackendForTesting(
    std::shared_ptr<LinuxQsvDriver>, const LinuxQsvDeviceSelection& selection);
}  // namespace webrtc::external_encoder::internal
#endif
