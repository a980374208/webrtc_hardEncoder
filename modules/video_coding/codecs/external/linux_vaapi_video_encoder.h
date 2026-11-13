/* Linux-only hardware H264 factory and immutable configuration. */
#ifndef MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_VAAPI_VIDEO_ENCODER_H_
#define MODULES_VIDEO_CODING_CODECS_EXTERNAL_LINUX_VAAPI_VIDEO_ENCODER_H_
#include <memory>
#include <string>
#include <string_view>
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/external/video_encoder_backend.h"
#include "modules/video_coding/codecs/external/linux_hardware_encoder_device.h"
namespace webrtc {
struct LinuxVaapiEncoderOptions {
  // Empty selects AMD first, then Intel, in stable device order. An explicit
  // render-node path never falls back to another GPU.
  std::string device_path;
  // Empty loads the packaged SONAME through the application's private RUNPATH.
  // A caller may supply an explicitly verified package-local runtime file.
  std::string runtime_path;
};
std::unique_ptr<VideoEncoder> CreateLinuxVaapiVideoEncoder(
    const Environment&, const SdpVideoFormat&,
    const LinuxVaapiEncoderOptions& options = {});
// Consumes the exact successfully retired probe identity; Open revalidates it.
std::unique_ptr<VideoEncoder> CreateLinuxVaapiVideoEncoder(
    const Environment&, const SdpVideoFormat&,
    const external_encoder::LinuxVaapiDeviceSelection&,
    const LinuxVaapiEncoderOptions& options = {});
external_encoder::Status ProbeLinuxVaapiVideoEncoderAvailability(
    const SdpVideoFormat&, const LinuxVaapiEncoderOptions& options = {},
    std::string* reason = nullptr);
external_encoder::Status ProbeLinuxVaapiVideoEncoderAvailability(
    const SdpVideoFormat&, const LinuxVaapiEncoderOptions&,
    std::string* reason, external_encoder::LinuxVaapiDeviceSelection* resolved);
namespace external_encoder {
// Linux VAAPI has its own process-lifetime state; it is never mapped to AMF.
Status CheckLinuxVaapiRetirement();
void RecordLinuxVaapiRetirement(std::string_view reason);
}  // namespace external_encoder
}  // namespace webrtc
#endif
