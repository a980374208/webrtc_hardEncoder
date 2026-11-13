/*
 * Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/video_coding/codecs/external/linux_qsv_driver.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

namespace webrtc::external_encoder::internal {
namespace {
template <typename Function>
bool Resolve(void* library, const char* name, Function* function) {
  void* symbol = dlsym(library, name);
  static_assert(sizeof(*function) == sizeof(symbol));
  std::memcpy(function, &symbol, sizeof(symbol));
  return *function != nullptr;
}
int OpenNode(const char* name) {
  const int fd = open(name, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -1;
  struct stat attributes{};
  if (fstat(fd, &attributes) != 0 || !S_ISCHR(attributes.st_mode)) {
    close(fd); return -1;
  }
  return fd;
}
std::shared_ptr<LinuxQsvDriver> Load(const char* vpl, const char* va,
    const char* va_drm, const char* drm, Status* status, std::string* reason) {
  *status = Status::kUnavailable;
  auto driver = std::make_shared<LinuxQsvDriver>();
  driver->vpl_library = dlopen(vpl, RTLD_NOW | RTLD_LOCAL);
  if (!driver->vpl_library) { *reason = "qsv_vpl_library_unavailable"; return nullptr; }
  driver->va_library = dlopen(va, RTLD_NOW | RTLD_LOCAL);
  if (!driver->va_library) { *reason = "qsv_va_library_unavailable"; return nullptr; }
  driver->va_drm_library = dlopen(va_drm, RTLD_NOW | RTLD_LOCAL);
  if (!driver->va_drm_library) { *reason = "qsv_va_drm_library_unavailable"; return nullptr; }
  driver->drm_library = dlopen(drm, RTLD_NOW | RTLD_LOCAL);
  if (!driver->drm_library) { *reason = "qsv_drm_library_unavailable"; return nullptr; }
#define VPL(field, symbol) Resolve(driver->vpl_library, symbol, &driver->field)
  const bool complete =
      VPL(load, "MFXLoad") && VPL(unload, "MFXUnload") &&
      VPL(create_config, "MFXCreateConfig") && VPL(filter, "MFXSetConfigFilterProperty") &&
      VPL(enumerate, "MFXEnumImplementations") &&
      VPL(release_description, "MFXDispReleaseImplDescription") &&
      VPL(create_session, "MFXCreateSession") && VPL(close_session, "MFXClose") &&
      VPL(query_impl, "MFXQueryIMPL") && VPL(query_version, "MFXQueryVersion") &&
      VPL(set_handle, "MFXVideoCORE_SetHandle") && VPL(get_handle, "MFXVideoCORE_GetHandle") &&
      VPL(query, "MFXVideoENCODE_Query") && VPL(query_surfaces, "MFXVideoENCODE_QueryIOSurf") &&
      VPL(initialize, "MFXVideoENCODE_Init") && VPL(reset, "MFXVideoENCODE_Reset") &&
      VPL(get_parameters, "MFXVideoENCODE_GetVideoParam") &&
      VPL(encode, "MFXVideoENCODE_EncodeFrameAsync") &&
      VPL(sync, "MFXVideoCORE_SyncOperation") && VPL(close_encoder, "MFXVideoENCODE_Close") &&
      Resolve(driver->va_library, "vaInitialize", &driver->initialize_va) &&
      Resolve(driver->va_library, "vaTerminate", &driver->terminate_va) &&
      Resolve(driver->va_drm_library, "vaGetDisplayDRM", &driver->display) &&
      Resolve(driver->drm_library, "drmGetDevices2", &driver->get_devices) &&
      Resolve(driver->drm_library, "drmFreeDevices", &driver->free_devices) &&
      Resolve(driver->drm_library, "drmGetDevice2", &driver->get_device) &&
      Resolve(driver->drm_library, "drmFreeDevice", &driver->free_device);
#undef VPL
  driver->open_node = OpenNode;
  driver->close_node = close;
  if (!complete || !driver->HasRequiredFunctions()) {
    *reason = "qsv_driver_entry_point_missing"; return nullptr;
  }
  *status = Status::kOk; reason->clear(); return driver;
}
}  // namespace
LinuxQsvDriver::~LinuxQsvDriver() {
  if (vpl_library) dlclose(vpl_library);
  if (va_drm_library) dlclose(va_drm_library);
  if (va_library) dlclose(va_library);
  if (drm_library) dlclose(drm_library);
}
bool LinuxQsvDriver::HasRequiredFunctions() const {
  return load && unload && create_config && filter && enumerate &&
      release_description && create_session && close_session && query_impl &&
      query_version && set_handle && get_handle && query && query_surfaces &&
      initialize && reset && get_parameters && encode && sync && close_encoder &&
      display && initialize_va && terminate_va && get_devices && free_devices &&
      get_device && free_device && open_node && close_node;
}
std::shared_ptr<LinuxQsvDriver> LoadLinuxQsvDriver(Status* status, std::string* reason) {
  return Load("libvpl.so.2", "libva.so.2", "libva-drm.so.2", "libdrm.so.2", status, reason);
}
std::shared_ptr<LinuxQsvDriver> LoadLinuxQsvDriverForTesting(
    const char* vpl, const char* va, const char* va_drm, const char* drm,
    Status* status, std::string* reason) {
  return Load(vpl, va, va_drm, drm, status, reason);
}
}  // namespace webrtc::external_encoder::internal
