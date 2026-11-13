/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_nvenc_driver.h"

#include <dlfcn.h>

#include <cstring>

namespace webrtc::external_encoder::internal {
namespace {
template <typename Function>
bool Resolve(void* library, const char* name, Function* function) {
  void* address = dlsym(library, name);
  static_assert(sizeof(*function) == sizeof(address));
  std::memcpy(function, &address, sizeof(address));
  return *function != nullptr;
}

std::shared_ptr<LinuxNvencDriver> Load(const char* cuda_library,
                                     const char* encode_library,
                                     Status* status,
                                     std::string* reason) {
  auto driver = std::make_shared<LinuxNvencDriver>();
  *status = Status::kUnavailable;
  driver->cuda_library = dlopen(cuda_library, RTLD_NOW | RTLD_LOCAL);
  if (!driver->cuda_library) {
    *reason = "cuda_library_unavailable";
    return nullptr;
  }
  bool cuda_complete =
      Resolve(driver->cuda_library, "cuInit", &driver->init) &&
      Resolve(driver->cuda_library, "cuDeviceGetCount", &driver->device_get_count) &&
      Resolve(driver->cuda_library, "cuDeviceGet", &driver->device_get) &&
      Resolve(driver->cuda_library, "cuDeviceGetPCIBusId", &driver->device_get_pci_bus_id) &&
      Resolve(driver->cuda_library, "cuCtxCreate_v2", &driver->context_create) &&
      Resolve(driver->cuda_library, "cuCtxGetCurrent", &driver->context_get_current) &&
      Resolve(driver->cuda_library, "cuCtxGetDevice", &driver->context_get_device) &&
      Resolve(driver->cuda_library, "cuCtxPushCurrent_v2", &driver->context_push) &&
      Resolve(driver->cuda_library, "cuCtxPopCurrent_v2", &driver->context_pop) &&
      Resolve(driver->cuda_library, "cuCtxDestroy_v2", &driver->context_destroy) &&
      Resolve(driver->cuda_library, "cuMemAllocPitch_v2", &driver->memory_alloc_pitch) &&
      Resolve(driver->cuda_library, "cuMemFree_v2", &driver->memory_free) &&
      Resolve(driver->cuda_library, "cuMemcpy2D_v2", &driver->copy_2d);
  if (!cuda_complete) {
    *reason = "cuda_entry_point_missing";
    return nullptr;
  }
  driver->encode_library = dlopen(encode_library, RTLD_NOW | RTLD_LOCAL);
  if (!driver->encode_library) {
    *reason = "nvenc_library_unavailable";
    return nullptr;
  }
  using GetVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
  using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
  GetVersion get_version = nullptr;
  CreateInstance create_instance = nullptr;
  if (!Resolve(driver->encode_library, "NvEncodeAPIGetMaxSupportedVersion", &get_version) ||
      !Resolve(driver->encode_library, "NvEncodeAPICreateInstance", &create_instance)) {
    *reason = "nvenc_entry_point_missing";
    return nullptr;
  }
  if (get_version(&driver->driver_api_version) != NV_ENC_SUCCESS) {
    *reason = "nvenc_api_version_query_failed";
    return nullptr;
  }
  constexpr uint32_t required =
      (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
  if (driver->driver_api_version < required) {
    *status = Status::kUnsupported;
    *reason = "nvenc_api_version_unsupported";
    return nullptr;
  }
  driver->encode.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (create_instance(&driver->encode) != NV_ENC_SUCCESS ||
      !driver->HasRequiredFunctions()) {
    *reason = "nvenc_function_table_incomplete";
    return nullptr;
  }
  *status = Status::kOk;
  reason->clear();
  return driver;
}
}  // namespace

LinuxNvencDriver::~LinuxNvencDriver() {
  if (encode_library) dlclose(encode_library);
  if (cuda_library) dlclose(cuda_library);
}

bool LinuxNvencDriver::HasRequiredFunctions() const {
  return init && device_get_count && device_get && device_get_pci_bus_id &&
         context_create && context_get_current && context_get_device &&
         context_push && context_pop && context_destroy && memory_alloc_pitch &&
         memory_free && copy_2d && encode.nvEncOpenEncodeSessionEx &&
         encode.nvEncGetEncodeGUIDCount && encode.nvEncGetEncodeGUIDs &&
         encode.nvEncGetEncodeProfileGUIDCount && encode.nvEncGetEncodeProfileGUIDs &&
         encode.nvEncGetInputFormatCount && encode.nvEncGetInputFormats &&
         encode.nvEncGetEncodePresetCount && encode.nvEncGetEncodePresetGUIDs &&
         encode.nvEncGetEncodeCaps && encode.nvEncGetEncodePresetConfigEx &&
         encode.nvEncInitializeEncoder && encode.nvEncRegisterResource &&
         encode.nvEncUnregisterResource && encode.nvEncMapInputResource &&
         encode.nvEncUnmapInputResource && encode.nvEncCreateBitstreamBuffer &&
         encode.nvEncDestroyBitstreamBuffer && encode.nvEncEncodePicture &&
         encode.nvEncLockBitstream && encode.nvEncUnlockBitstream &&
         encode.nvEncReconfigureEncoder && encode.nvEncDestroyEncoder;
}

std::shared_ptr<LinuxNvencDriver> LoadLinuxNvencDriver(Status* status,
                                                     std::string* reason) {
  return Load("libcuda.so.1", "libnvidia-encode.so.1", status, reason);
}

std::shared_ptr<LinuxNvencDriver> LoadLinuxNvencDriverForTesting(
    const char* cuda_library,
    const char* encode_library,
    Status* status,
    std::string* reason) {
  return Load(cuda_library, encode_library, status, reason);
}
}  // namespace webrtc::external_encoder::internal
