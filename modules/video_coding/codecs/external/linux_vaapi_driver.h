/* Linux-private runtime dispatch and fake-driver seam. */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_VAAPI_DRIVER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_VAAPI_DRIVER_H_
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "modules/video_coding/codecs/external/linux_vaapi_runtime.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device_internal.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"
namespace webrtc::external_encoder::internal {
struct LinuxVaapiDriver {
  LinuxVaapiDriver() = default;
  ~LinuxVaapiDriver();
  LinuxVaapiDriver(const LinuxVaapiDriver&) = delete;
  LinuxVaapiDriver& operator=(const LinuxVaapiDriver&) = delete;
  bool HasRequiredFunctions() const;
  CohavoraVaapiApi api{};
  void* library = nullptr;
};
struct LinuxVaapiDevice {
  std::string path;
  std::string pci;
  uint32_t vendor = 0;
  uint32_t device = 0;
};
bool ValidLinuxVaapiSelection(const LinuxVaapiDeviceSelection&);
bool SameLinuxVaapiSelection(const LinuxVaapiDeviceSelection&,
                            const LinuxVaapiDeviceSelection&);
std::optional<LinuxVaapiDeviceSelection> LinuxVaapiSelection(
    const LinuxVaapiDevice&);
// Open and identify the actual render-node FD before the runtime can use it.
// The runtime opens /proc/self/fd/<pin>, so a renamed node cannot swap GPUs.
Status PinLinuxVaapiDevice(const LinuxVaapiDevice&, int* owned_fd,
                          std::string* reason);
std::shared_ptr<LinuxVaapiDriver> LoadLinuxVaapiDriver(
    const std::string& runtime_path, Status* status, std::string* reason);
std::vector<LinuxVaapiDevice> EnumerateLinuxVaapiDevices();
std::unique_ptr<VideoEncoderBackend> CreateLinuxVaapiBackendForTesting(
    std::shared_ptr<LinuxVaapiDriver>, std::vector<LinuxVaapiDevice>,
    std::string requested_device = {},
    std::optional<LinuxVaapiDeviceSelection> expected = std::nullopt,
    std::shared_ptr<LinuxVaapiDeviceCollector> collector = {});
}  // namespace webrtc::external_encoder::internal
#endif
