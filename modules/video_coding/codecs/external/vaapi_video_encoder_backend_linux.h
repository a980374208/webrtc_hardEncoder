#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_VAAPI_VIDEO_ENCODER_BACKEND_LINUX_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_VAAPI_VIDEO_ENCODER_BACKEND_LINUX_H_
#include <memory>
#include <string>
#include "modules/video_coding/codecs/external/video_encoder_backend.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device_internal.h"
namespace webrtc::external_encoder {
// Inert until Open. Driver operations are synchronous; consumers requiring a
// deadline must use the Linux bounded owner, which also owns borrowed input.
std::unique_ptr<VideoEncoderBackend> CreateLinuxVaapiBackend(
    std::string device_path = {}, std::string runtime_path = {},
    std::shared_ptr<internal::LinuxVaapiDeviceCollector> collector = {});
std::unique_ptr<VideoEncoderBackend> CreateLinuxVaapiBackend(
    const LinuxVaapiDeviceSelection& selection, std::string runtime_path = {},
    std::shared_ptr<internal::LinuxVaapiDeviceCollector> collector = {});
Status CheckLinuxVaapiRetirement();
void RecordLinuxVaapiRetirement(std::string_view reason);
}  // namespace webrtc::external_encoder
#endif
